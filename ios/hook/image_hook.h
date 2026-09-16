/* Included by hook.c. Unity APIs run only from managed UI callbacks, never dyld.
 * Use runtime_invoke for value-type arguments/returns instead of guessing the
 * native ARM64 ABI of Rect/Vector2/Vector4. Reference arguments are object pointers.
 */
typedef void (*ImageSetter)(void *, void *, const void *);
typedef void (*ImageEnable)(void *, const void *);
static ImageSetter original_image_sprite, original_image_override, original_image_texture;
static ImageEnable original_image_enable;
static char image_root[4096];
static int image_busy, image_api_state, image_exception;
static unsigned image_hits;
static void *(*image_invoke)(const void *, void *, void **, void **);
static void *(*image_unbox)(void *);
static void *(*image_object_new)(void *);
static void *(*image_array_new)(void *, uintptr_t);
static uint32_t (*image_root_new)(void *, uint8_t);
static void (*image_root_free)(uint32_t);
static void *(*image_root_get)(uint32_t);
static void *image_byte_class, *image_texture_class;
static const void *image_ctor, *image_load, *image_create, *image_hide, *image_wrap;
static const void *image_rect, *image_pivot, *image_border, *image_ppu, *image_alive;
static const void *image_get_sprite, *image_get_texture, *image_aspect, *image_destroy;

struct ImageRect { float x, y, width, height; };
struct ImageVec2 { float x, y; };
struct ImageVec4 { float x, y, z, w; };
struct ImageCache {
    char name[512];
    uint32_t texture, sprite;
    int missing;
    struct ImageCache *next;
};
static struct ImageCache *image_cache;

static void *image_call(const void *method, void *self, void **args) {
    if (!method || image_exception) return 0;
    void *exception = 0;
    void *result = image_invoke(method, self, args, &exception);
    if (exception) {
        image_exception = 1;
        record("IMAGE EXCEPTION: method=%p; retaining original image", method);
        return 0;
    }
    return result;
}

/* Resolve exact parameter types, not just an overloaded method's name/count. */
static const void *image_method(void *klass, const char *name, unsigned count,
                                const char **types) {
    typedef const void *(*Methods)(void *, void **);
    typedef const char *(*Name)(const void *);
    typedef uint32_t (*Count)(const void *);
    typedef const void *(*Param)(const void *, uint32_t);
    typedef char *(*TypeName)(const void *);
    typedef void (*Free)(void *);
    Methods methods = (Methods)dlsym(unity_handle, "il2cpp_class_get_methods");
    Name method_name = (Name)dlsym(unity_handle, "il2cpp_method_get_name");
    Count param_count = (Count)dlsym(unity_handle, "il2cpp_method_get_param_count");
    Param param = (Param)dlsym(unity_handle, "il2cpp_method_get_param");
    TypeName type_name = (TypeName)dlsym(unity_handle, "il2cpp_type_get_name");
    Free release = (Free)dlsym(unity_handle, "il2cpp_free");
    if (!klass || !methods || !method_name || !param_count || !param || !type_name || !release) return 0;
    void *iter = 0;
    const void *method;
    while ((method = methods(klass, &iter))) {
        if (strcmp(method_name(method), name) || param_count(method) != count) continue;
        unsigned i;
        for (i = 0; i < count; ++i) {
            char *actual = type_name(param(method, i));
            int match = actual && !strcmp(actual, types[i]);
            if (actual) release(actual);
            if (!match) break;
        }
        if (i == count) return method;
    }
    record("IMAGE API MISSING: %s argc=%u", name, count);
    return 0;
}

static int image_resolve(void) {
    if (image_api_state) return image_api_state > 0;
    image_api_state = -1;
    typedef void *(*Domain)(void);
    typedef const void **(*Assemblies)(void *, size_t *);
    typedef const void *(*AssemblyImage)(const void *);
    typedef const char *(*ImageName)(const void *);
    typedef void *(*Class)(const void *, const char *, const char *);
    Domain domain = (Domain)dlsym(unity_handle, "il2cpp_domain_get");
    Assemblies assemblies = (Assemblies)dlsym(unity_handle, "il2cpp_domain_get_assemblies");
    AssemblyImage get_image = (AssemblyImage)dlsym(unity_handle, "il2cpp_assembly_get_image");
    ImageName name = (ImageName)dlsym(unity_handle, "il2cpp_image_get_name");
    Class klass = (Class)dlsym(unity_handle, "il2cpp_class_from_name");
#define IMAGE_API(field, symbol) *(void **)(&field) = dlsym(unity_handle, symbol)
    IMAGE_API(image_invoke, "il2cpp_runtime_invoke");
    IMAGE_API(image_unbox, "il2cpp_object_unbox");
    IMAGE_API(image_object_new, "il2cpp_object_new");
    IMAGE_API(image_array_new, "il2cpp_array_new");
    IMAGE_API(image_root_new, "il2cpp_gchandle_new");
    IMAGE_API(image_root_free, "il2cpp_gchandle_free");
    IMAGE_API(image_root_get, "il2cpp_gchandle_get_target");
#undef IMAGE_API
    if (!domain || !assemblies || !get_image || !name || !klass || !image_invoke ||
        !image_unbox || !image_object_new || !image_array_new || !image_root_new ||
        !image_root_free || !image_root_get) goto fail;
    const void *core = 0, *conversion = 0, *ui = 0, *system = 0;
    size_t count = 0;
    void *d = domain();
    const void **list = d ? assemblies(d, &count) : 0;
    for (size_t i = 0; list && i < count; ++i) {
        const void *img = get_image(list[i]);
        const char *n = img ? name(img) : 0;
        if (!n) continue;
        if (!strcmp(n, "UnityEngine.CoreModule.dll")) core = img;
        if (!strcmp(n, "UnityEngine.ImageConversionModule.dll")) conversion = img;
        if (!strcmp(n, "UnityEngine.UI.dll")) ui = img;
        if (!strcmp(n, "mscorlib.dll")) system = img;
    }
    if (!core || !conversion || !ui || !system) goto fail;
    void *sprite = klass(core, "UnityEngine", "Sprite");
    void *texture = klass(core, "UnityEngine", "Texture");
    void *object = klass(core, "UnityEngine", "Object");
    void *image = klass(ui, "UnityEngine.UI", "Image");
    void *raw = klass(ui, "UnityEngine.UI", "RawImage");
    image_byte_class = klass(system, "System", "Byte");
    image_texture_class = klass(core, "UnityEngine", "Texture2D");
    const char *ctor[] = {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"};
    const char *load[] = {"UnityEngine.Texture2D", "System.Byte[]", "System.Boolean"};
    const char *create[] = {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2", "System.Single", "System.UInt32", "UnityEngine.SpriteMeshType", "UnityEngine.Vector4"};
    const char *hide[] = {"UnityEngine.HideFlags"}, *wrap[] = {"UnityEngine.TextureWrapMode"};
    const char *boolean[] = {"System.Boolean"}, *obj[] = {"UnityEngine.Object"};
    image_ctor = image_method(image_texture_class, ".ctor", 4, ctor);
    image_load = image_method(klass(conversion, "UnityEngine", "ImageConversion"), "LoadImage", 3, load);
    image_create = image_method(sprite, "Create", 7, create);
    image_hide = image_method(object, "set_hideFlags", 1, hide);
    image_wrap = image_method(texture, "set_wrapMode", 1, wrap);
    image_alive = image_method(object, "op_Implicit", 1, obj);
    image_destroy = image_method(object, "Destroy", 1, obj);
    image_rect = image_method(sprite, "get_rect", 0, 0);
    image_pivot = image_method(sprite, "get_pivot", 0, 0);
    image_border = image_method(sprite, "get_border", 0, 0);
    image_ppu = image_method(sprite, "get_pixelsPerUnit", 0, 0);
    image_get_sprite = image_method(image, "get_sprite", 0, 0);
    image_get_texture = image_method(raw, "get_texture", 0, 0);
    image_aspect = image_method(image, "set_preserveAspect", 1, boolean);
    if (!image_byte_class || !image_texture_class || !image_ctor || !image_load || !image_create ||
        !image_hide || !image_wrap || !image_alive || !image_destroy || !image_rect || !image_pivot ||
        !image_border || !image_ppu || !image_get_sprite || !image_get_texture || !image_aspect) goto fail;
    image_api_state = 1;
    record("IMAGE API READY: Texture2D/LoadImage/Sprite.Create; Android image replacement");
    return 1;
fail:
    record("IMAGE DISABLED: required IL2CPP/Unity APIs unavailable; text translation remains active");
    return 0;
}

static int image_name(void *object, char name[512]) {
    void *managed = object && font_get_name ? font_get_name(object, 0) : 0;
    size_t used = 0;
    if (!managed || !append_managed_utf8(name, 512, &used, managed)) return 0;
    if (used >= 7 && !memcmp(name + used - 7, "(Clone)", 7)) name[used -= 7] = 0;
    if (!used || strstr(name, "..") || name[0] == '/' || name[0] == '\\') return 0;
    for (size_t i = 0; i < used; ++i) if (name[i] == ':' || name[i] == '\\') return 0;
    return 1;
}

static unsigned image_be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static unsigned char *image_read(const char *name, size_t *size, unsigned *w, unsigned *h) {
    char path[4608];
    FILE *file = 0;
    for (int i = 0; i < 2 && !file; ++i) {
        int n = snprintf(path, sizeof(path), "%s/%s%s", image_root, name, i ? "" : ".png");
        if (n <= 0 || (size_t)n >= sizeof(path)) return 0;
        file = fopen(path, "rb");
    }
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 0; }
    long n = ftell(file);
    if (n < 33 || n > 32 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    unsigned char *bytes = malloc((size_t)n);
    if (!bytes) { fclose(file); return 0; }
    size_t read = fread(bytes, 1, (size_t)n, file);
    fclose(file);
    if (read != (size_t)n || memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) ||
        image_be32(bytes + 8) != 13 || memcmp(bytes + 12, "IHDR", 4)) { free(bytes); return 0; }
    *w = image_be32(bytes + 16); *h = image_be32(bytes + 20);
    if (!*w || !*h || *w > 16384 || *h > 16384 || (uint64_t)*w * *h > 64 * 1024 * 1024) {
        free(bytes); return 0;
    }
    *size = read;
    return bytes;
}

static int image_value(const void *method, void *self, void *out, size_t size) {
    void *boxed = image_call(method, self, 0);
    void *value = boxed ? image_unbox(boxed) : 0;
    if (!value) return 0;
    memcpy(out, value, size);
    return 1;
}

static void image_persist(void *obj) {
    int32_t flags = 32; /* Android: DontUnloadUnusedAsset */
    void *args[] = {&flags}; image_call(image_hide, obj, args);
}

static void *image_cached(uint32_t *handle) {
    if (!*handle) return 0;
    void *value = image_root_get(*handle);
    void *args[] = {value};
    void *boxed = value ? image_call(image_alive, 0, args) : 0;
    uint8_t *alive = boxed ? image_unbox(boxed) : 0;
    if (alive && *alive) return value;
    image_root_free(*handle); *handle = 0;
    return 0;
}

static void *image_replacement(void *original_asset, int want_sprite) {
    char name[512];
    if (!image_root[0] || !image_name(original_asset, name)) return 0;
    struct ImageCache *cache = image_cache;
    while (cache && strcmp(cache->name, name)) cache = cache->next;
    if (cache && cache->missing) return 0;
    if (!cache) {
        cache = malloc(sizeof(*cache));
        if (!cache) return 0;
        memcpy(cache->name, name, strlen(name) + 1);
        cache->texture = cache->sprite = 0; cache->missing = 0;
        cache->next = image_cache; image_cache = cache;
    }
    if (!image_resolve()) return 0;
    void *cached = image_cached(want_sprite ? &cache->sprite : &cache->texture);
    if (cached || image_exception) return cached;
    size_t size = 0; unsigned w = 0, h = 0;
    unsigned char *bytes = image_read(name, &size, &w, &h);
    if (!bytes) { cache->missing = 1; return 0; }
    void *tex = image_cached(&cache->texture);
    if (!tex && !image_exception) {
        void *array = image_array_new(image_byte_class, size);
        uint32_t array_root = array ? image_root_new(array, 0) : 0;
        if (!array_root) { free(bytes); return 0; }
        /* Version-locked IL2CPP array header: object, monitor, bounds, length. */
        memcpy((char *)array + 4 * sizeof(void *), bytes, size);
        tex = image_object_new(image_texture_class);
        uint32_t tex_root = tex ? image_root_new(tex, 0) : 0;
        if (!tex_root) { image_root_free(array_root); free(bytes); return 0; }
        int32_t two = 2, format = 4, clamp = 1; uint8_t no = 0;
        void *ctor[] = {&two, &two, &format, &no};
        if (image_hits < 3) record("IMAGE CREATE TEXTURE: name=%s bytes=%llu", name, (unsigned long long)size);
        image_call(image_ctor, tex, ctor);
        void *load[] = {tex, array, &no};
        if (image_hits < 3) record("IMAGE LOAD PNG: %s", name);
        void *boxed = image_call(image_load, 0, load);
        uint8_t *success = boxed ? image_unbox(boxed) : 0;
        int loaded = success && *success;
        image_root_free(array_root);
        if (loaded && !image_exception) {
            void *wrap[] = {&clamp}; image_call(image_wrap, tex, wrap);
            image_persist(tex);
        }
        if (!loaded || image_exception) {
            image_exception = 0;
            void *destroy[] = {tex}; image_call(image_destroy, 0, destroy);
            image_root_free(tex_root); free(bytes);
            record("IMAGE LOAD FAILED: %s", name); return 0;
        }
        cache->texture = tex_root;
    }
    free(bytes);
    if (!tex || image_exception) return 0;
    void *result = tex;
    if (want_sprite) {
        struct ImageRect old_rect, rect = {0, 0, (float)w, (float)h};
        struct ImageVec2 old_pivot, pivot = {0.5f, 0.5f};
        struct ImageVec4 border;
        float ppu;
        if (!image_value(image_rect, original_asset, &old_rect, sizeof(old_rect)) ||
            !image_value(image_pivot, original_asset, &old_pivot, sizeof(old_pivot)) ||
            !image_value(image_border, original_asset, &border, sizeof(border)) ||
            !image_value(image_ppu, original_asset, &ppu, sizeof(ppu))) return 0;
        if (old_rect.width > 0 && old_rect.height > 0) {
            pivot.x = old_pivot.x / old_rect.width; pivot.y = old_pivot.y / old_rect.height;
        }
        uint32_t extrude = 0; int32_t mesh = 0;
        void *args[] = {tex, &rect, &pivot, &ppu, &extrude, &mesh, &border};
        if (image_hits < 3) record("IMAGE CREATE SPRITE: %s", name);
        result = image_call(image_create, 0, args);
        if (!result || image_exception) return 0;
        uint32_t root = image_root_new(result, 0);
        if (!root) return 0;
        image_persist(result);
        if (image_exception) { image_root_free(root); return 0; }
        cache->sprite = root;
    }
    ++image_hits;
    if (image_hits <= 40 || image_hits % 100 == 0)
        record("IMAGE REPLACED: hit=%u name=%s kind=%s size=%ux%u", image_hits, name,
               want_sprite ? "Sprite" : "Texture", w, h);
    return result;
}

static void image_preserve_aspect(void *self) {
    uint8_t yes = 1; void *args[] = {&yes}; image_call(image_aspect, self, args);
}

static void image_set(void *self, void *value, const void *method, ImageSetter original_set, int sprite) {
    if (!images_enabled || image_busy || !value) { original_set(self, value, method); return; }
    image_busy = 1; image_exception = 0;
    void *replacement = image_replacement(value, sprite);
    original_set(self, replacement ? replacement : value, method);
    if (replacement && sprite) image_preserve_aspect(self);
    image_busy = 0;
}
static void image_sprite_hook(void *self, void *value, const void *method) {
    image_set(self, value, method, original_image_sprite, 1);
}
static void image_override_hook(void *self, void *value, const void *method) {
    image_set(self, value, method, original_image_override, 1);
}
static void image_texture_hook(void *self, void *value, const void *method) {
    image_set(self, value, method, original_image_texture, 0);
}
static void image_enable_hook(void *self, const void *method) {
    original_image_enable(self, method);
    if (!images_enabled || image_busy || !self) return;
    void *klass = font_object_class(self);
    const char *name = klass ? class_get_name(klass) : 0;
    int sprite = name && !strcmp(name, "Image");
    if (!sprite && (!name || strcmp(name, "RawImage"))) return;
    image_busy = 1; image_exception = 0;
    if (image_resolve()) {
        void *value = image_call(sprite ? image_get_sprite : image_get_texture, self, 0);
        void *replacement = value && !image_exception ? image_replacement(value, sprite) : 0;
        if (replacement) {
            (sprite ? original_image_sprite : original_image_texture)(self, replacement, 0);
            if (sprite) image_preserve_aspect(self);
        }
    }
    image_busy = 0;
}
