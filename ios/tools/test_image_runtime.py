"""Run the production image pipeline with mocked IL2CPP/Unity APIs on ARM64."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_LR
from test_generic_runtime import SUPPORT, ROOT

MOCKS = r'''
typedef void FILE;
static int scenario, ctor_count, load_count, create_count, applied, aspect_count, errors;
static char texture_object, sprite_object, original_object, owner_object;
static void *roots[16]; static uint32_t root_count;
static void *unity_handle; static int images_enabled=1;
static void record(const char *fmt, ...) { (void)fmt; }
static void *dlsym(void *h,const char *s) { (void)h;(void)s;return 0; }
static char *strstr(const char *s,const char *t) { for(;*s;s++) if(!memcmp(s,t,strlen(t)))return (char*)s;return 0; }
static int snprintf(char *d,size_t n,const char *fmt,...) { (void)n;(void)fmt; memcpy(d,"icon.png",9);return 8; }
static FILE *fopen(const char *p,const char *m) { (void)p;(void)m;return (FILE*)1; }
static int fclose(FILE *f) {(void)f;return 0;}
static int fseek(FILE *f,long n,int mode) {(void)f;(void)n;(void)mode;return 0;}
static long ftell(FILE *f) {(void)f;return 33;}
static size_t fread(void *b,size_t s,size_t n,FILE *f) {
 (void)s;(void)f; unsigned char png[33]={0x89,'P','N','G',13,10,26,10,0,0,0,13,'I','H','D','R',0,0,0,64,0,0,0,32};
 if(scenario==3)png[0]=0;memcpy(b,png,33);return n;
}
#define SEEK_END 2
#define SEEK_SET 0
static void *font_get_name(void *v,const void *m) {(void)v;(void)m;return "icon(Clone)";}
static int append_managed_utf8(char *d,size_t n,size_t *used,void *s) {
 *used=strlen(s);if(*used>=n)return 0;memcpy(d,s,*used+1);return 1;
}
static void *font_object_class(void *v) {return v;}
static const char *class_get_name(void *v) {(void)v;return "Image";}
'''

DRIVER = r'''
static void *mock_new(void *klass) {(void)klass;return &texture_object;}
static void *mock_array(void *klass,uintptr_t n) {(void)klass;return malloc(n+32);}
static uint32_t mock_root(void *v,uint8_t pin) {(void)pin; roots[++root_count]=v;return root_count;}
static void mock_free(uint32_t h) {roots[h]=0;}
static void *mock_get(uint32_t h) {return roots[h];}
static void *mock_unbox(void *v) {return v;}
static void *mock_invoke(const void *m,void *self,void **args,void **exception) {
 static uint8_t yes=1;static struct ImageRect rect={10,20,100,200};
 static struct ImageVec2 pivot={25,150};static struct ImageVec4 border={1,2,3,4};static float ppu=100;
 switch((uintptr_t)m) {
 case 1: ctor_count++;if(self!=&texture_object || *(int*)args[0]!=2 || *(int*)args[2]!=4 || *(uint8_t*)args[3])errors++;break;
 case 2: load_count++;if(args[0]!=&texture_object || ((unsigned char*)args[1])[32]!=0x89)errors++;
         if(scenario==4){*exception=(void*)1;return 0;} return &yes;
 case 3: {create_count++;struct ImageRect *r=args[1];struct ImageVec2 *p=args[2];struct ImageVec4 *b=args[6];
   if(args[0]!=&texture_object || r->x!=0 || r->width!=64 || r->height!=32 || p->x!=0.25f || p->y!=0.75f || *(float*)args[3]!=100 || *(int*)args[4] || *(int*)args[5] || b->w!=4)errors++;
   return &sprite_object;}
 case 4: if(*(int*)args[0]!=32)errors++;break;
 case 5: if(*(int*)args[0]!=1)errors++;break;
 case 6: return &rect;
 case 7: return &pivot;
 case 8: return &border;
 case 9: return &ppu;
 case 10:return &yes;
 case 11:return &original_object;
 case 12:return &original_object;
 case 13:aspect_count++;if(*(uint8_t*)args[0]!=1)errors++;break;
 case 14:break;
 default: errors++;
 }return 0;
}
static void mock_set(void *self,void *value,const void *method) {
 (void)self;(void)method; applied = value==&sprite_object ? 1 : value==&texture_object ? 2 : 0;
}
static void mock_enable(void *self,const void *method) {(void)self;(void)method;}
__attribute__((section(".entry"))) int *run(int mode) {
 static int result[6];scenario=mode;heap=0x800000;image_api_state=1;image_root[0]='x';
 image_invoke=mock_invoke;image_unbox=mock_unbox;image_object_new=mock_new;image_array_new=mock_array;
 image_root_new=mock_root;image_root_free=mock_free;image_root_get=mock_get;
 image_ctor=(void*)1;image_load=(void*)2;image_create=(void*)3;image_hide=(void*)4;image_wrap=(void*)5;
 image_rect=(void*)6;image_pivot=(void*)7;image_border=(void*)8;image_ppu=(void*)9;image_alive=(void*)10;
 image_get_sprite=(void*)11;image_get_texture=(void*)12;image_aspect=(void*)13;image_destroy=(void*)14;
 original_image_sprite=mock_set;original_image_override=mock_set;original_image_texture=mock_set;original_image_enable=mock_enable;
 if(mode==2)images_enabled=0;
 if(mode==5)image_enable_hook(&owner_object,0);
 else if(mode==1)image_texture_hook(&owner_object,&original_object,0);
 else {image_sprite_hook(&owner_object,&original_object,0);image_sprite_hook(&owner_object,&original_object,0);}
 result[0]=ctor_count;result[1]=load_count;result[2]=create_count;result[3]=applied;result[4]=aspect_count;result[5]=errors;
 return result;
}
'''


class ImageRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        llvm = Path(os.environ.get("HOSHIMI_LLVM_BIN", str(Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk/ndk/26.3.11579264/toolchains/llvm/prebuilt/windows-x86_64/bin")))
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "build")
        path = Path(cls.temp.name)
        support = SUPPORT[:SUPPORT.index("/* The extracted translator")]
        code = support + MOCKS + (ROOT / "hook/image_hook.h").read_text() + DRIVER
        (path / "test.c").write_text(code, encoding="utf-8")
        (path / "link.ld").write_text("SECTIONS { . = 0x100000; .text : { *(.entry) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }")
        subprocess.run([str(llvm / "clang.exe"), "--target=aarch64-none-elf", "-ffreestanding", "-fno-stack-protector", "-O1", "-c", str(path / "test.c"), "-o", str(path / "test.o")], check=True)
        subprocess.run([str(llvm / "ld.lld.exe"), "-T", str(path / "link.ld"), "--oformat=binary", str(path / "test.o"), "-o", str(path / "test.bin")], check=True)
        cls.code = (path / "test.bin").read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, mode):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x100000, 0x1000000)
        uc.mem_write(0x100000, self.code)
        uc.reg_write(UC_ARM64_REG_X0, mode)
        uc.reg_write(UC_ARM64_REG_SP, 0x700000)
        uc.reg_write(UC_ARM64_REG_LR, 0x200000)
        uc.emu_start(0x100000, 0x200000, count=2000000)
        return struct.unpack("<6i", bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X0), 24)))

    def test_sprite_geometry_and_cache(self):
        self.assertEqual(self.run_case(0), (1, 1, 1, 1, 2, 0))

    def test_raw_texture(self):
        self.assertEqual(self.run_case(1), (1, 1, 0, 2, 0, 0))

    def test_disabled_retains_original(self):
        self.assertEqual(self.run_case(2), (0, 0, 0, 0, 0, 0))

    def test_bad_png_retains_original(self):
        self.assertEqual(self.run_case(3), (0, 0, 0, 0, 0, 0))

    def test_managed_exception_retains_original(self):
        self.assertEqual(self.run_case(4), (2, 2, 0, 0, 0, 0))

    def test_onenable_replaces_serialized_sprite(self):
        self.assertEqual(self.run_case(5), (1, 1, 1, 1, 1, 0))
