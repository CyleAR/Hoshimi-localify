"""Execute the production generic translator as ARM64 code, without an iPad.

Set HOSHIMI_LLVM_BIN to the NDK LLVM bin directory. Requires unicorn.
The harness supplies only allocation/libc; translation code comes from hook.c.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_SP, UC_ARM64_REG_LR

ROOT = Path(__file__).resolve().parents[1]
SUPPORT = r'''
#include <stddef.h>
#include <stdint.h>
static uintptr_t heap;
void *malloc(size_t n) { void *p=(void *)heap; heap+=(n+15)&~15UL; return p; }
void free(void *p) { (void)p; }
size_t strlen(const char *s) { size_t n=0; while(s[n]) ++n; return n; }
void *memcpy(void *d,const void *s,size_t n) { for(size_t i=0;i<n;i++) ((char*)d)[i]=((const char*)s)[i]; return d; }
int memcmp(const void *a,const void *b,size_t n) { for(size_t i=0;i<n;i++) { int d=((const unsigned char*)a)[i]-((const unsigned char*)b)[i]; if(d) return d; } return 0; }
int strcmp(const char *a,const char *b) { while(*a && *a==*b) { a++; b++; } return (unsigned char)*a-(unsigned char)*b; }
/* The extracted translator uses snprintf only for "%u}" with index 0..31. */
int snprintf(char *s,size_t n,const char *fmt,unsigned value) { (void)n; (void)fmt; int k=0; if(value>=10)s[k++]='0'+value/10; s[k++]='0'+value%10; s[k++]='}'; s[k]=0; return k; }
static void *generic_blob;
static size_t generic_blob_size;
static unsigned adv_dash_replacements, adv_josa_replacements;
'''


def make_blob():
    groups = [{"原文": "번역", "失敗12": "실패12"},
              {"残り{0}日": "{0}일 남음", "broken{0}": "partial{9}"},
              {"未受取": "미수령"}, {"번역": "", "실패12": ""}]
    pool = bytearray(b"\0")
    rows = []
    for group in groups:
        for key, value in sorted(group.items(), key=lambda item: item[0].encode()):
            offsets = []
            for text in (key, value):
                offsets.append(len(pool))
                pool.extend(text.encode() + b"\0")
            rows.append(struct.pack("<4I", *offsets, len(key.encode()), len(value.encode())))
    offset = 36 + 16 * len(rows)
    return struct.pack("<8s7I", b"HSGEN2", 2, *(len(g) for g in groups), offset, offset + len(pool)) + b"".join(rows) + pool


class GenericRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        llvm = Path(os.environ.get("HOSHIMI_LLVM_BIN", str(Path(os.environ.get("LOCALAPPDATA", "")) / "Android/Sdk/ndk/26.3.11579264/toolchains/llvm/prebuilt/windows-x86_64/bin")))
        source = (ROOT / "hook/hook.c").read_text(encoding="utf-8")
        norm = source[source.index("static int josa_batchim("):source.index("static char *read_adv_text(")]
        generic = source[source.index("struct GenericHeader {"):source.index("static void generic_apply_center_layout(")]
        a, b = generic.index("static int load_generic_index("), generic.index("static size_t generic_flag_length(")
        generic = generic[:a] + generic[b:]
        wrapper = '''
__attribute__((section(".entry"))) char *run(char *input, void *blob, size_t size) {
 heap=0x800000; generic_blob=blob; generic_blob_size=size;
 size_t length=0; int translated=0;
 return generic_translate_utf8(input,strlen(input),&length,&translated,1);
}
'''
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "build")
        path = Path(cls.temp.name)
        (path / "test.c").write_text(SUPPORT + norm + generic + wrapper, encoding="utf-8")
        (path / "link.ld").write_text("SECTIONS { . = 0x100000; .text : { *(.entry) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }")
        subprocess.run([str(llvm / "clang.exe"), "--target=aarch64-none-elf", "-ffreestanding", "-fno-stack-protector", "-O1", "-c", str(path / "test.c"), "-o", str(path / "test.o")], check=True)
        subprocess.run([str(llvm / "ld.lld.exe"), "-T", str(path / "link.ld"), "--oformat=binary", str(path / "test.o"), "-o", str(path / "test.bin")], check=True)
        cls.code = (path / "test.bin").read_bytes()
        cls.blob = make_blob()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def translate(self, text):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(0x100000, 0x1000000)
        uc.mem_write(0x100000, self.code)
        uc.mem_write(0x300000, text.encode() + b"\0")
        uc.mem_write(0x400000, self.blob)
        for reg, value in [(UC_ARM64_REG_X0, 0x300000), (UC_ARM64_REG_X1, 0x400000), (UC_ARM64_REG_X2, len(self.blob)), (UC_ARM64_REG_SP, 0x700000), (UC_ARM64_REG_LR, 0x200000)]:
            uc.reg_write(reg, value)
        uc.emu_start(0x100000, 0x200000, count=2000000)
        self.assertNotEqual(uc.reg_read(UC_ARM64_REG_X0), 0)
        return bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X0), 4096)).split(b"\0")[0].decode()

    def test_untranslated_numeric_and_tagged_text_never_duplicates(self):
        for text in ["13920", "G3", "12:00 시작", "5일 남음", "<b>Dummy.</b>", "plain", ""]:
            with self.subTest(text=text):
                result = text
                for _ in range(4):
                    result = self.translate(result)
                    self.assertEqual(result, text)

    def test_translation_paths_and_repeated_calls(self):
        for source, expected in [("原文", "번역"), ("原文++", "번역++"), ("残り5日", "5일 남음"), ("<b>未受取</b>", "<b>미수령</b>"), ("失敗12", "실패12"), ("broken12", "broken12"), ("⸺", "—"), ("사람[은/는]", "사람은"), ("하루[이/가]", "하루가")]:
            with self.subTest(source=source):
                result = self.translate(source)
                self.assertEqual(result, expected)
                self.assertEqual(self.translate(result), expected)


if __name__ == "__main__":
    unittest.main()
