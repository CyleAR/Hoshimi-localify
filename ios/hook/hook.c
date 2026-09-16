/* Version-locked I18n.SetValue port. The IPA contains the executable branch
 * and trampoline already; runtime setup writes only a dedicated data pointer. */
#include "../probe/darwin_abi.h"
#include "hook_profile.h"
#include "translations.h"

typedef void (*SetValue)(void *, void *, void *, const void *);
typedef int32_t (*StringLength)(void *);
typedef const uint16_t *(*StringChars)(void *);
typedef void *(*StringNew)(const uint16_t *, int32_t);
typedef void *(*StringNewUtf8)(const char *);
static SetValue original;
static StringLength string_length;
static StringChars string_chars;
static StringNew string_new;
static StringNewUtf8 string_new_utf8;
static FILE *log_file;
static unsigned calls, hits;
static int armed;
static int patch_enabled = 1;
static int diagnostics_enabled;
static int master_enabled = 1;
static int images_enabled = 1;
static int phone_subtitles_enabled = 1;
#ifndef HOSHIMI_TEXT_ONLY
static void *unity_handle;
typedef void (*AdvLoad)(void *, void *, void *, void *, const void *);
static AdvLoad original_adv_load;
static char adv_root[4096];
static unsigned adv_calls, adv_hits;
static unsigned adv_dash_replacements, adv_josa_replacements;
typedef void (*MasterMerge)(void *, void *, void *);
typedef const char *(*ClassGetName)(void *);
static MasterMerge original_master_merge;
static ClassGetName class_get_name;
static char master_path[4096];
static char generic_path[4096];
static void *master_blob;
static size_t master_blob_size;
static unsigned master_calls, master_hits;
static void *generic_blob;
static size_t generic_blob_size;
static unsigned generic_calls, generic_hits;

typedef void (*TmpSetText)(void *, void *, const void *);
typedef void (*TmpSetTextBool)(void *, void *, uint8_t, const void *);
typedef void (*TmpPopulateText)(void *, void *, int32_t, int32_t, const void *);
typedef void (*TmpSetCharArray)(void *, void *, int32_t, int32_t, const void *);
typedef void (*UiSetText)(void *, void *, const void *);
static TmpSetText original_tmp_set_text;
static TmpPopulateText original_tmp_populate_text;
static TmpSetTextBool original_tmp_settext_bool;
static TmpSetCharArray original_tmp_setchararray;
static UiSetText original_textfield_set_value;
static UiSetText original_ui_text_set_text;
static int append_managed_utf8(char *output, size_t capacity, size_t *used, void *value);
static void *method_entry(const void *method);
#endif

static void record(const char *format, ...) {
    if (!log_file) return;
    flockfile(log_file);
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);
    funlockfile(log_file);
}

/* Settings.bundle writes this key into the current app's preferences domain.
 * Resolve CoreFoundation dynamically so the SDK-free Windows build stays
 * linkable with only libSystem. Missing settings always mean enabled. */
static int read_boolean_setting(const char *setting_key, int default_value) {
    typedef void *(*StringCreate)(void *, const char *, uint32_t);
    typedef uint8_t (*GetBoolean)(void *, void *, uint8_t *);
    typedef void (*Release)(void *);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                      RTLD_NOW);
    if (!cf) return default_value;
    StringCreate make_string = (StringCreate)dlsym(cf, "CFStringCreateWithCString");
    GetBoolean get_boolean = (GetBoolean)dlsym(cf, "CFPreferencesGetAppBooleanValue");
    Release release = (Release)dlsym(cf, "CFRelease");
    void **current_application = (void **)dlsym(cf, "kCFPreferencesCurrentApplication");
    if (!make_string || !get_boolean || !release || !current_application ||
        !*current_application) {
        dlclose(cf);
        return default_value;
    }
    void *key = make_string(0, setting_key, 0x08000100u);
    if (!key) { dlclose(cf); return default_value; }
    uint8_t exists = 0;
    uint8_t value = get_boolean(key, *current_application, &exists);
    release(key);
    dlclose(cf);
    return exists ? value != 0 : default_value;
}

static const struct Translation *lookup(const uint16_t *key, uint32_t length) {
    unsigned low = 0, high = TRANSLATION_COUNT;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        const struct Translation *t = translations + mid;
        const uint16_t *candidate = translation_pool + t->key;
        uint32_t n = length < t->key_len ? length : t->key_len;
        int order = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (key[i] != candidate[i]) { order = key[i] < candidate[i] ? -1 : 1; break; }
        }
        if (!order) order = length < t->key_len ? -1 : (length > t->key_len ? 1 : 0);
        if (!order) return t;
        if (order < 0) high = mid;
        else low = mid + 1;
    }
    return 0;
}

static void key_preview(const uint16_t *key, uint32_t length, char output[129]) {
    unsigned n = length < 128 ? length : 128;
    for (unsigned i = 0; i < n; ++i)
        output[i] = key[i] >= 32 && key[i] <= 126 ? (char)key[i] : '?';
    output[n] = 0;
}

#ifndef HOSHIMI_TEXT_ONLY
typedef void (*FontAwake)(void *, const void *);
typedef void *(*FontGetName)(void *, const void *);
typedef void (*FontSetBool)(void *, uint8_t, const void *);
typedef void *(*FontGetFallbackTable)(void *, const void *);
typedef void *(*FontObjectClass)(void *);
typedef const void *(*FontMethodFromName)(void *, const char *, int);

static FontAwake original_font_awake;
static FontGetName font_get_name;
static FontSetBool font_set_multi_atlas;
static FontSetBool font_clear_data;
static FontGetFallbackTable font_get_fallback_table;
static FontObjectClass font_object_class;
static FontMethodFromName font_method_from_name;
static void *source_sans_asset;
static void *solis_asset;
static void *observed_fonts[512];
static uint8_t fallback_injected[512];
static size_t observed_font_count;
static unsigned font_awake_count;
static int font_activated;
#ifdef HOSHIMI_DISCOVERY
static uintptr_t unity_base;
static int metadata_probe_started;

struct ProbeTarget {
    const char *image;
    const char *namespaze;
    const char *klass;
};

static int wanted_probe_method(const char *klass, const char *name) {
    if (!name) return 0;
    if (!strcmp(klass, "TMP_Text"))
        return !strcmp(name, "set_text") || !strcmp(name, "SetText") ||
               !strcmp(name, "PopulateTextBackingArray") || !strcmp(name, "SetCharArray");
    if (!strcmp(klass, "TextMeshProUGUI")) return !strcmp(name, "Awake");
    if (!strcmp(klass, "Text")) return !strcmp(name, "set_text");
    if (!strcmp(klass, "MessageExtensions")) return !strcmp(name, "MergeFrom");
    if (!strcmp(klass, "OctoResourceLoader")) return !strcmp(name, "LoadFromCacheOrDownload");
    return 0;
}

static void *probe_translation_targets(void *unused) {
    (void)unused;
    typedef void *(*NoArg)(void);
    typedef const void **(*Assemblies)(void *, size_t *);
    typedef void *(*PointerArg)(void *);
    typedef void (*VoidArg)(void *);
    typedef const char *(*Name)(const void *);
    typedef void *(*ClassFromName)(const void *, const char *, const char *);
    typedef const void *(*Methods)(void *, void **);
    typedef uint32_t (*ParamCount)(const void *);
    typedef const void *(*Param)(const void *, uint32_t);
    typedef char *(*TypeName)(const void *);
    NoArg domain_get = (NoArg)dlsym(unity_handle, "il2cpp_domain_get");
    Assemblies assemblies_get = (Assemblies)dlsym(unity_handle, "il2cpp_domain_get_assemblies");
    PointerArg thread_attach = (PointerArg)dlsym(unity_handle, "il2cpp_thread_attach");
    VoidArg thread_detach = (VoidArg)dlsym(unity_handle, "il2cpp_thread_detach");
    PointerArg image_get = (PointerArg)dlsym(unity_handle, "il2cpp_assembly_get_image");
    Name image_name = (Name)dlsym(unity_handle, "il2cpp_image_get_name");
    ClassFromName class_get = (ClassFromName)dlsym(unity_handle, "il2cpp_class_from_name");
    Methods methods_get = (Methods)dlsym(unity_handle, "il2cpp_class_get_methods");
    Name method_name = (Name)dlsym(unity_handle, "il2cpp_method_get_name");
    ParamCount param_count = (ParamCount)dlsym(unity_handle, "il2cpp_method_get_param_count");
    Param param_get = (Param)dlsym(unity_handle, "il2cpp_method_get_param");
    TypeName type_name = (TypeName)dlsym(unity_handle, "il2cpp_type_get_name");
    VoidArg il2cpp_free = (VoidArg)dlsym(unity_handle, "il2cpp_free");
    if (!domain_get || !assemblies_get || !thread_attach || !thread_detach || !image_get ||
        !image_name || !class_get || !methods_get || !method_name || !param_count ||
        !param_get || !type_name || !il2cpp_free) {
        record("DISCOVERY FAIL: required IL2CPP metadata exports missing");
        return 0;
    }
    static const struct ProbeTarget targets[] = {
        {"Unity.TextMeshPro.dll", "TMPro", "TMP_Text"},
        {"Unity.TextMeshPro.dll", "TMPro", "TextMeshProUGUI"},
        {"UnityEngine.UI.dll", "UnityEngine.UI", "Text"},
        {"Google.Protobuf.dll", "Google.Protobuf", "MessageExtensions"},
        {"Octo.dll", "Octo.Loader", "OctoResourceLoader"},
    };
    for (int attempt = 0; attempt < 120; ++attempt) {
        void *domain = domain_get();
        size_t assembly_count = 0;
        const void **assemblies = domain ? assemblies_get(domain, &assembly_count) : 0;
        if (!assemblies || !assembly_count) { sleep(1); continue; }
        void *thread = thread_attach(domain);
        if (!thread) { sleep(1); continue; }
        unsigned found = 0;
        for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
            void *image = 0;
            for (size_t i = 0; i < assembly_count; ++i) {
                void *candidate = image_get((void *)assemblies[i]);
                const char *candidate_name = candidate ? image_name(candidate) : 0;
                if (candidate_name && !strcmp(candidate_name, targets[t].image)) {
                    image = candidate;
                    break;
                }
            }
            void *klass = image ? class_get(image, targets[t].namespaze, targets[t].klass) : 0;
            if (!klass) continue;
            void *iter = 0;
            const void *method = 0;
            while ((method = methods_get(klass, &iter))) {
                const char *name = method_name(method);
                if (!wanted_probe_method(targets[t].klass, name)) continue;
                void *entry = 0;
                memcpy(&entry, method, sizeof(entry));
                uint32_t count = param_count(method);
                if (!entry || (uintptr_t)entry < unity_base) continue;
                record("DISCOVERY TARGET %s.%s params=%u rva=0x%llx",
                       targets[t].klass, name, count,
                       (unsigned long long)((uintptr_t)entry - unity_base));
                for (uint32_t p = 0; p < count; ++p) {
                    char *type = type_name(param_get(method, p));
                    record("  parameter[%u]=%s", p, type ? type : "<unknown>");
                    if (type) il2cpp_free(type);
                }
                ++found;
            }
        }
        thread_detach(thread);
        if (found) {
            record("DISCOVERY PASS: %u candidate methods recorded", found);
            return 0;
        }
        sleep(1);
    }
    record("DISCOVERY FAIL: translation targets not found within 120 seconds");
    return 0;
}
#endif

static void *font_entry(const void *method) {
    void *entry = 0;
    if (method) memcpy(&entry, method, sizeof(entry));
    return entry;
}

static int managed_string_is(void *value, const char *wanted) {
    if (!value || !wanted || !string_length || !string_chars) return 0;
    int32_t length = string_length(value);
    size_t wanted_length = strlen(wanted);
    if (length < 0 || (size_t)length != wanted_length) return 0;
    const uint16_t *chars = string_chars(value);
    if (!chars) return 0;
    for (size_t i = 0; i < wanted_length; ++i)
        if (chars[i] != (uint16_t)(unsigned char)wanted[i]) return 0;
    return 1;
}

static size_t track_font(void *font) {
    for (size_t i = 0; i < observed_font_count; ++i)
        if (observed_fonts[i] == font) return i;
    if (observed_font_count >= sizeof(observed_fonts) / sizeof(observed_fonts[0]))
        return (size_t)-1;
    observed_fonts[observed_font_count] = font;
    return observed_font_count++;
}

static int insert_font_first(void *list, void *font) {
    if (!list || !font || !font_object_class || !font_method_from_name) return 0;
    void *klass = font_object_class(list);
    if (!klass) return 0;
    const void *method = font_method_from_name(klass, "Insert", 2);
    void *entry = font_entry(method);
    if (entry) {
        typedef void (*ListInsert)(void *, int32_t, void *, const void *);
        ((ListInsert)entry)(list, 0, font, method);
        return 1;
    }
    method = font_method_from_name(klass, "Add", 1);
    entry = font_entry(method);
    if (!entry) return 0;
    typedef void (*ListAdd)(void *, void *, const void *);
    ((ListAdd)entry)(list, font, method);
    return 1;
}

static void inject_observed_fallbacks(void) {
    if (!font_activated || !font_get_fallback_table) return;
    unsigned added = 0;
    for (size_t i = 0; i < observed_font_count; ++i) {
        void *asset = observed_fonts[i];
        if (!asset || asset == source_sans_asset || fallback_injected[i]) continue;
        void *list = font_get_fallback_table(asset, 0);
        if (!list || !insert_font_first(list, source_sans_asset)) continue;
        fallback_injected[i] = 1;
        ++added;
    }
    if (added) record("FONT FALLBACK: SourceSansPro inserted into %u observed assets", added);
}

static void activate_awake_font(void) {
    if (font_activated || !source_sans_asset || !solis_asset) return;
    const size_t face_info_offset = 0x50;
    const size_t metric_offset = 0x18;
    const size_t metric_size = 0x60 - metric_offset;
    memcpy((char *)source_sans_asset + face_info_offset + metric_offset,
           (const char *)solis_asset + face_info_offset + metric_offset, metric_size);
    record("FONT FACEINFO: copied Solis-MK5 metrics at field offset 0x50");
    font_set_multi_atlas(source_sans_asset, 1, 0);
    font_clear_data(source_sans_asset, 0, 0);
    font_activated = 1;
    record("FONT ACTIVATE: SourceSansPro-Regular SDF cleared for dynamic Korean glyphs");
    inject_observed_fallbacks();
}

static void font_awake_hook(void *self, const void *method) {
    size_t index = track_font(self);
    original_font_awake(self, method);
    unsigned call = ++font_awake_count;
    void *name = font_get_name ? font_get_name(self, 0) : 0;
    if (call <= 32) {
        char name_text[129] = "<null>";
        int32_t name_length = name ? string_length(name) : 0;
        const uint16_t *name_chars = name ? string_chars(name) : 0;
        if (name_chars && name_length >= 0) key_preview(name_chars, (uint32_t)name_length, name_text);
        record("FONT AWAKE: call=%u asset=%p name=%s", call, self, name_text);
    }
    if (managed_string_is(name, "SourceSansPro-Regular SDF")) {
        source_sans_asset = self;
        record("FONT CAPTURE: SourceSansPro-Regular SDF=%p", self);
    } else if (!solis_asset && managed_string_is(name, "Solis-MK5 SDF")) {
        solis_asset = self;
        record("FONT CAPTURE: Solis-MK5 SDF=%p", self);
    }
    activate_awake_font();
    if (font_activated && index != (size_t)-1 && !fallback_injected[index])
        inject_observed_fallbacks();
}

#if 0 /* superseded: scanning all Resources at translation call 64 was unsafe */
/* The Android built-in-font mode replaces SourceSansPro-Regular on disk and
 * then registers that TMP_FontAsset as a global fallback. The iOS package has
 * the same replaced Font object, so no AssetBundle is loaded at runtime. */
typedef void *(*FontDomainGet)(void);
typedef const void **(*FontAssembliesGet)(void *, size_t *);
typedef void *(*FontAssemblyImage)(const void *);
typedef const char *(*FontImageName)(const void *);
typedef void *(*FontClassFromName)(const void *, const char *, const char *);
typedef const void *(*FontMethodFromName)(void *, const char *, int);
typedef void *(*FontRuntimeInvoke)(const void *, void *, void **, void **);
typedef void *(*FontObjectClass)(void *);
typedef const void *(*FontClassGetType)(void *);
typedef void *(*FontTypeGetObject)(const void *);
typedef const void *(*FontFieldFromName)(void *, const char *);
typedef int32_t (*FontFieldOffset)(const void *);

/* Keep every metadata lookup observable.  On this Unity build the exported
 * resolver is valid, but a lookup can still be unsafe until the containing
 * image has finished registration.  The before/after pair tells us whether
 * the failure is in the resolver itself or in the subsequent managed call. */
static const void *font_lookup_method(FontMethodFromName method_from_name,
                                      void *klass, const char *owner,
                                      const char *name, int argc) {
    if (!method_from_name || !klass) {
        record("FONT METHOD SKIP: %s.%s argc=%d class=%p", owner, name, argc, klass);
        return 0;
    }
    record("FONT METHOD BEGIN: %s.%s argc=%d class=%p", owner, name, argc, klass);
    const void *method = method_from_name(klass, name, argc);
    record("FONT METHOD DONE: %s.%s argc=%d method=%p", owner, name, argc, method);
    return method;
}

/* MethodInfo starts with the generated ARM64 entry point on this IL2CPP
 * build.  A few UnityEngine static methods are unsafe through
 * il2cpp_runtime_invoke while the first scene is still loading, so callers
 * can use this only after validating the resolver result. */
static void *font_method_entry(const void *method) {
    void *entry = 0;
    if (method) memcpy(&entry, method, sizeof(entry));
    return entry;
}

/* IL2CPP reference arrays have this layout on the arm64 Unity build used by
 * IDOLY PRIDE.  Using the layout avoids depending on generic array exports. */
typedef struct FontObjectArray {
    void *klass;
    void *monitor;
    void *bounds;
    uintptr_t max_length;
    void *vector[1];
} FontObjectArray;

static void *font_find_image(FontDomainGet domain_get, FontAssembliesGet assemblies_get,
                             FontAssemblyImage image_get, FontImageName image_name,
                             const char *wanted) {
    void *domain = domain_get ? domain_get() : 0;
    size_t count = 0;
    const void **assemblies = domain ? assemblies_get(domain, &count) : 0;
    if (!assemblies) return 0;
    for (size_t i = 0; i < count; ++i) {
        void *image = image_get(assemblies[i]);
        const char *name = image ? image_name(image) : 0;
        if (name && strcmp(name, wanted) == 0) return image;
    }
    return 0;
}

static int font_string_equals_ascii(void *managed_string, const char *wanted) {
    if (!managed_string || !wanted || !string_length || !string_chars) return 0;
    int32_t length = string_length(managed_string);
    size_t wanted_length = strlen(wanted);
    if (length < 0 || (size_t)length != wanted_length) return 0;
    const uint16_t *chars = string_chars(managed_string);
    if (!chars) return 0;
    for (size_t i = 0; i < wanted_length; ++i)
        if (chars[i] != (uint16_t)(unsigned char)wanted[i]) return 0;
    return 1;
}

static int font_object_named(void *object, const void *get_name,
                             FontRuntimeInvoke invoke, const char *wanted) {
    if (!object || !get_name || !invoke) return 0;
    void *exception = 0;
    void *name = invoke(get_name, object, 0, &exception);
    return !exception && font_string_equals_ascii(name, wanted);
}

static void *font_find_loaded_assets(void *core_image, void *tmp_image,
                                     FontClassFromName class_from_name,
                                     FontMethodFromName method_from_name,
                                     FontClassGetType class_get_type,
                                     FontTypeGetObject type_get_object,
                                     FontRuntimeInvoke invoke) {
    (void)invoke;
    void *resources = core_image ? class_from_name(core_image, "UnityEngine", "Resources") : 0;
    void *tmp_font = tmp_image ? class_from_name(tmp_image, "TMPro", "TMP_FontAsset") : 0;
    record("FONT STEP: Resources class=%p TMP class=%p", resources, tmp_font);
    const void *find_all = font_lookup_method(method_from_name, resources,
                                              "UnityEngine.Resources", "FindObjectsOfTypeAll", 1);
    record("FONT TYPE BEGIN: il2cpp_class_get_type class=%p", tmp_font);
    const void *font_type = tmp_font && class_get_type ? class_get_type(tmp_font) : 0;
    record("FONT TYPE DONE: Il2CppType=%p", font_type);
    record("FONT TYPE OBJECT BEGIN: il2cpp_type_get_object type=%p", font_type);
    void *type_object = font_type && type_get_object ? type_get_object(font_type) : 0;
    record("FONT TYPE OBJECT DONE: System.Type=%p", type_object);
    record("FONT STEP: TMP type=%p System.Type=%p FindObjectsOfTypeAll=%p",
           font_type, type_object, find_all);
    if (!find_all || !type_object) return 0;
    record("FONT STEP: calling Resources.FindObjectsOfTypeAll directly");
    void *entry = font_method_entry(find_all);
    if (!entry) {
        record("FONT FAIL: Resources.FindObjectsOfTypeAll has no method entry");
        return 0;
    }
    typedef void *(*FindObjectsOfTypeAll)(void *, const void *);
    /* Generated signature: (System.Type* type, const MethodInfo* method). */
    void *result = ((FindObjectsOfTypeAll)entry)(type_object, find_all);
    record("FONT STEP: Resources.FindObjectsOfTypeAll returned %p", result);
    return result;
}

static int font_add_to_list(void *list, void *font, FontObjectClass object_class,
                            FontMethodFromName method_from_name, FontRuntimeInvoke invoke) {
    if (!list || !font) return 0;
    void *list_class = object_class ? object_class(list) : 0;
    const void *insert = font_lookup_method(method_from_name, list_class,
                                            "System.Collections.Generic.List", "Insert", 2);
    const void *add = font_lookup_method(method_from_name, list_class,
                                         "System.Collections.Generic.List", "Add", 1);
    void *font_arg = font;
    void *exception = 0;
    if (insert) {
        int32_t index = 0;
        void *args[2] = {&index, &font_arg};
        invoke(insert, list, args, &exception);
    } else if (add) {
        void *args[1] = {&font_arg};
        invoke(add, list, args, &exception);
    } else {
        return 0;
    }
    return exception == 0;
}

static int activate_static_font(void *source, void *solis, void *tmp_class,
                                FontMethodFromName method_from_name,
                                FontFieldFromName field_from_name,
                                FontFieldOffset field_offset,
                                FontRuntimeInvoke invoke) {
    if (!source || !solis || !tmp_class) return 0;

    const void *set_multi = font_lookup_method(method_from_name, tmp_class,
                                               "TMPro.TMP_FontAsset", "set_isMultiAtlasTexturesEnabled", 1);
    const void *clear_data = font_lookup_method(method_from_name, tmp_class,
                                                "TMPro.TMP_FontAsset", "ClearFontAssetData", 1);
    if (!set_multi || !clear_data) {
        record("FONT FAIL: TMP activation methods not found");
        return 0;
    }

    /* Match the Android replacement path: preserve managed references and
     * copy only the numeric part of FaceInfo from Solis-MK5. */
    const void *face_field = field_from_name ? field_from_name(tmp_class, "m_FaceInfo") : 0;
    int32_t face_offset = face_field && field_offset ? field_offset(face_field) : -1;
    if (face_offset > 0) {
        const size_t metric_offset = 0x18;
        const size_t metric_size = 0x60 - metric_offset;
        memcpy((char *)source + face_offset + metric_offset,
               (const char *)solis + face_offset + metric_offset, metric_size);
        record("FONT FACEINFO: copied Solis-MK5 metrics to SourceSansPro-Regular");
    } else {
        record("FONT WARN: m_FaceInfo field offset unavailable");
    }

    uint8_t enabled = 1;
    void *multi_args[1] = {&enabled};
    void *exception = 0;
    invoke(set_multi, source, multi_args, &exception);
    if (exception) {
        record("FONT FAIL: enabling multi-atlas raised an exception");
        return 0;
    }

    uint8_t set_atlas_size_zero = 0;
    void *clear_args[1] = {&set_atlas_size_zero};
    exception = 0;
    invoke(clear_data, source, clear_args, &exception);
    if (exception) {
        record("FONT FAIL: ClearFontAssetData raised an exception");
        return 0;
    }
    record("FONT ACTIVATE: SourceSansPro-Regular SDF dynamic data cleared");
    return 1;
}

static int register_static_font_fallback(void) {
    if (!unity_handle) return 0;
    record("FONT START: resolving IL2CPP/TMP APIs handle=%p", unity_handle);
#define FONT_RESOLVE(type, variable, symbol) \
    type variable = (type)dlsym(unity_handle, symbol); \
    record("FONT RESOLVE: %s=%s", symbol, variable ? "yes" : "no")
    FONT_RESOLVE(FontDomainGet, domain_get, "il2cpp_domain_get");
    FONT_RESOLVE(FontAssembliesGet, assemblies_get, "il2cpp_domain_get_assemblies");
    FONT_RESOLVE(FontAssemblyImage, image_get, "il2cpp_assembly_get_image");
    FONT_RESOLVE(FontImageName, image_name, "il2cpp_image_get_name");
    FONT_RESOLVE(FontClassFromName, class_from_name, "il2cpp_class_from_name");
    FONT_RESOLVE(FontMethodFromName, method_from_name, "il2cpp_class_get_method_from_name");
    FONT_RESOLVE(FontRuntimeInvoke, invoke, "il2cpp_runtime_invoke");
    FONT_RESOLVE(FontObjectClass, object_class, "il2cpp_object_get_class");
    FONT_RESOLVE(FontClassGetType, class_get_type, "il2cpp_class_get_type");
    FONT_RESOLVE(FontTypeGetObject, type_get_object, "il2cpp_type_get_object");
    FONT_RESOLVE(FontFieldFromName, field_from_name, "il2cpp_class_get_field_from_name");
    FONT_RESOLVE(FontFieldOffset, field_offset, "il2cpp_field_get_offset");
#undef FONT_RESOLVE
    record("FONT STEP: exports resolved domain=%s assemblies=%s invoke=%s",
           domain_get ? "yes" : "no", assemblies_get ? "yes" : "no",
           invoke ? "yes" : "no");
    if (!domain_get || !assemblies_get || !image_get || !image_name || !class_from_name ||
        !method_from_name || !invoke || !object_class || !class_get_type || !type_get_object) {
        record("FONT FAIL: fallback IL2CPP exports are missing");
        return 0;
    }
    void *tmp_image = font_find_image(domain_get, assemblies_get, image_get, image_name,
                                      "Unity.TextMeshPro.dll");
    record("FONT STEP: TMP image=%p", tmp_image);
    void *tmp_class = tmp_image ? class_from_name(tmp_image, "TMPro", "TMP_FontAsset") : 0;
    void *settings = tmp_image ? class_from_name(tmp_image, "TMPro", "TMP_Settings") : 0;
    void *core_image = font_find_image(domain_get, assemblies_get, image_get, image_name,
                                       "UnityEngine.CoreModule.dll");
    void *object_class_def = core_image ? class_from_name(core_image, "UnityEngine", "Object") : 0;
    record("FONT STEP: core image=%p TMP class=%p settings=%p Object class=%p",
           core_image, tmp_class, settings, object_class_def);
    const void *get_name = font_lookup_method(method_from_name, object_class_def,
                                              "UnityEngine.Object", "get_name", 0);
    const void *fallback_getter = font_lookup_method(method_from_name, settings,
                                                     "TMPro.TMP_Settings", "get_fallbackFontAssets", 0);
    const void *font_fallback_getter = font_lookup_method(method_from_name, tmp_class,
                                                          "TMPro.TMP_FontAsset", "get_fallbackFontAssetTable", 0);
    if (!tmp_class || !get_name || !fallback_getter || !font_fallback_getter) {
        record("FONT FAIL: TMP font/fallback methods not found"); return 0;
    }

    record("FONT STEP: TMP method lookup complete name=%p globalFallback=%p perFontFallback=%p",
           get_name, fallback_getter, font_fallback_getter);
    void *assets = font_find_loaded_assets(core_image, tmp_image, class_from_name,
                                           method_from_name, class_get_type, type_get_object, invoke);
    if (!assets) {
        record("FONT WAIT: Resources.FindObjectsOfTypeAll returned no TMP assets");
        return 0;
    }
    record("FONT STEP: enumerating loaded TMP_FontAsset objects");

    FontObjectArray *array = (FontObjectArray *)assets;
    if (array->max_length > 20000) {
        record("FONT FAIL: unreasonable TMP asset array length=%llu",
               (unsigned long long)array->max_length);
        return 0;
    }

    void *source = 0;
    void *solis = 0;
    size_t asset_count = 0;
    for (uintptr_t i = 0; i < array->max_length; ++i) {
        void *asset = array->vector[i];
        if (!asset) continue;
        ++asset_count;
        if (!source && font_object_named(asset, get_name, invoke, "SourceSansPro-Regular SDF")) source = asset;
        if (!solis && font_object_named(asset, get_name, invoke, "Solis-MK5 SDF")) solis = asset;
    }
    if (!source || !solis) {
        record("FONT WAIT: SourceSansPro-Regular SDF=%p Solis-MK5 SDF=%p loaded=%llu",
               source, solis, (unsigned long long)asset_count);
        return 0;
    }
    record("FONT FOUND: SourceSansPro-Regular SDF=%p Solis-MK5 SDF=%p",
           source, solis);

    static void *activated_source;
    static void *activated_solis;
    if (source != activated_source || solis != activated_solis) {
        if (!activate_static_font(source, solis, tmp_class, method_from_name,
                                  field_from_name, field_offset, invoke)) return 0;
        activated_source = source;
        activated_solis = solis;
    }

    size_t registered = 0;
    void *exception = 0;
    void *global_list = invoke(fallback_getter, 0, 0, &exception);
    if (!exception && global_list && font_add_to_list(global_list, source, object_class,
                                                       method_from_name, invoke)) {
        ++registered;
    }

    /* TMP resolves a character through the active asset's own table before
     * consulting TMP_Settings. Register the patched asset on every loaded
     * TMP_FontAsset, matching Android UpdateFont(). */
    for (uintptr_t i = 0; i < array->max_length; ++i) {
        void *asset = array->vector[i];
        if (!asset || asset == source) continue;
        exception = 0;
        void *table = invoke(font_fallback_getter, asset, 0, &exception);
        if (exception || !table) continue;
        if (font_add_to_list(table, source, object_class, method_from_name, invoke)) ++registered;
    }

    if (!registered) {
        record("FONT FAIL: patched asset could not be added to any TMP fallback list");
        return 0;
    }
    record("FONT OK: SourceSansPro-Regular SDF activated; fallback lists updated=%llu",
           (unsigned long long)registered);
    return 1;
}

static void static_font_fallback_main(void *unused) {
    (void)unused;
    record("FONT MAIN: static fallback activation dispatched to main queue");
    if (register_static_font_fallback())
        __atomic_store_n(&font_fallback_state, 1, __ATOMIC_RELEASE);
    else
        __atomic_store_n(&font_fallback_state, 0, __ATOMIC_RELEASE);
}

static void ensure_static_font_fallback(void) {
    int state = __atomic_load_n(&font_fallback_state, __ATOMIC_ACQUIRE);
    if (state == 1) return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&font_fallback_state, &expected, 2, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    /* UnityEngine Resources/TMP APIs must run on Unity's main thread.  The
     * translation callback can arrive from an async managed worker, whereas
     * Android performs this work from TMP_FontAsset.Awake on the main thread. */
    dispatch_async_f(dispatch_get_main_queue(), 0, static_font_fallback_main);
}
#endif
#endif

static void set_value_hook(void *self, void *key, void *value, const void *method) {
    unsigned call = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (call == 1) record("SETVALUE ENTER: native translation callback reached");
    const struct Translation *match = 0;
    const uint16_t *chars = 0;
    int32_t length = key ? string_length(key) : 0;
    if (key && length >= 0 && length <= 65536) {
        chars = string_chars(key);
        if (chars) match = lookup(chars, (uint32_t)length);
    }
    if (match) {
        void *translated = string_new(translation_pool + match->value, (int32_t)match->value_len);
        if (translated) {
            unsigned hit = __atomic_add_fetch(&hits, 1, __ATOMIC_RELAXED);
            if (hit <= 24 || hit == 100 || hit == 1000) {
                char key_text[129];
                key_preview(chars, (uint32_t)length, key_text);
                record("TRANSLATED hit=%u key=%s utf16_length=%u", hit, key_text, match->value_len);
            }
            /* Preserve the hidden MethodInfo argument as well as self/key.
             * The new managed string stays on the current managed-call stack. */
            original(self, key, translated, method);
            return;
        }
        record("WARN: managed string allocation returned null; keeping original value");
    }
    if (call <= 8) {
        char key_text[129] = "<null>";
        if (chars) key_preview(chars, (uint32_t)length, key_text);
        record("ORIGINAL call=%u key=%s", call, key_text);
    }
    original(self, key, value, method);
}

#ifndef HOSHIMI_TEXT_ONLY
static int resource_name_ascii(void *managed, char output[256]) {
    if (!managed || !string_length || !string_chars) return 0;
    int32_t length = string_length(managed);
    if (length <= 0 || length >= 256) return 0;
    const uint16_t *chars = string_chars(managed);
    if (!chars) return 0;
    for (int32_t i = 0; i < length; ++i) {
        uint16_t c = chars[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
        output[i] = (char)c;
    }
    output[length] = 0;
    return 1;
}

static int josa_batchim(uint32_t code) {
    if (code < 0xac00 || code > 0xd7a3) return -1;
    return (int)((code - 0xac00) % 28);
}

static int josa_special_batchim(const char *text, size_t end) {
    if (!end) return -1;
    char last = text[end - 1];
    if (last >= '0' && last <= '9') {
        static const int digits[] = {21, 8, 0, 16, 0, 0, 1, 8, 8, 0};
        return digits[last - '0'];
    }
    size_t start = end;
    while (start) {
        char c = text[start - 1];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) break;
        --start;
    }
    if (start == end) return -1;
    size_t length = end - start;
    if (length == 4 &&
        ((text[start] == 'f' || text[start] == 'F') &&
         (text[start + 1] == 'r' || text[start + 1] == 'R') &&
         (text[start + 2] == 'a' || text[start + 2] == 'A') &&
         (text[start + 3] == 'n' || text[start + 3] == 'N'))) return 21;
    if ((length == 4 || length == 3) &&
        ((length == 4 && (text[start] == 'm' || text[start] == 'M') &&
          (text[start + 1] == 'i' || text[start + 1] == 'I') &&
          (text[start + 2] == 'h' || text[start + 2] == 'H') &&
          (text[start + 3] == 'o' || text[start + 3] == 'O')) ||
         (length == 4 && (text[start] == 'k' || text[start] == 'K') &&
          (text[start + 1] == 'a' || text[start + 1] == 'A') &&
          (text[start + 2] == 'n' || text[start + 2] == 'N') &&
          (text[start + 3] == 'a' || text[start + 3] == 'A')))) return 0;
    return -1;
}

static uint32_t utf8_previous_codepoint(const char *text, size_t end) {
    if (!end) return 0;
    size_t start = end - 1;
    while (start && ((unsigned char)text[start] & 0xc0) == 0x80) --start;
    unsigned char first = (unsigned char)text[start];
    if (first < 0x80) return first;
    if ((first & 0xe0) == 0xc0 && start + 1 < end)
        return ((uint32_t)(first & 0x1f) << 6) |
               ((unsigned char)text[start + 1] & 0x3f);
    if ((first & 0xf0) == 0xe0 && start + 2 < end)
        return ((uint32_t)(first & 0x0f) << 12) |
               ((uint32_t)((unsigned char)text[start + 1] & 0x3f) << 6) |
               ((unsigned char)text[start + 2] & 0x3f);
    if ((first & 0xf8) == 0xf0 && start + 3 < end)
        return ((uint32_t)(first & 0x07) << 18) |
               ((uint32_t)((unsigned char)text[start + 1] & 0x3f) << 12) |
               ((uint32_t)((unsigned char)text[start + 2] & 0x3f) << 6) |
               ((unsigned char)text[start + 3] & 0x3f);
    return first;
}

static const char *josa_replacement(const char *tag, size_t tag_length,
                                    int has_batchim, int batchim) {
#define TAG(literal) (tag_length == sizeof(literal) - 1 && !memcmp(tag, literal, sizeof(literal) - 1))
    if (TAG("[\xec\x9d\x80/\xeb\x8a\x94]"))
        return has_batchim ? "\xec\x9d\x80" : "\xeb\x8a\x94";
    if (TAG("[\xec\x9d\xb4/\xea\xb0\x80]"))
        return has_batchim ? "\xec\x9d\xb4" : "\xea\xb0\x80";
    if (TAG("[\xec\x9d\xb4/\xeb\x9e\x91]") || TAG("[\xec\x9d\xb4\xeb\x9e\x91/\xeb\x9e\x91]"))
        return has_batchim ? "\xec\x9d\xb4\xeb\x9e\x91" : "\xeb\x9e\x91";
    if (TAG("[\xec\x9d\xb4/\xeb\x9d\xbc]") || TAG("[\xec\x9d\xb4\xeb\x9d\xbc/\xeb\x9d\xbc]"))
        return has_batchim ? "\xec\x9d\xb4\xeb\x9d\xbc" : "\xeb\x9d\xbc";
    if (TAG("[\xec\x9d\xb4/\xeb\x8b\xa4]") || TAG("[\xec\x9d\xb4\xeb\x8b\xa4/\xeb\x8b\xa4]"))
        return has_batchim ? "\xec\x9d\xb4\xeb\x8b\xa4" : "\xeb\x8b\xa4";
    if (TAG("[\xec\x99\x80/\xea\xb3\xbc]") || TAG("[\xea\xb3\xbc/\xec\x99\x80]"))
        return has_batchim ? "\xea\xb3\xbc" : "\xec\x99\x80";
    if (TAG("[\xec\x9d\x84/\xeb\xa5\xbc]"))
        return has_batchim ? "\xec\x9d\x84" : "\xeb\xa5\xbc";
    if (TAG("[\xec\x95\x84/\xec\x95\xbc]"))
        return has_batchim ? "\xec\x95\x84" : "\xec\x95\xbc";
    if (TAG("[\xec\x9c\xbc/\xeb\xa1\x9c]") || TAG("[\xec\x9c\xbc\xeb\xa1\x9c/\xeb\xa1\x9c]"))
        return batchim == 8 || !has_batchim ? "\xeb\xa1\x9c" : "\xec\x9c\xbc\xeb\xa1\x9c";
#undef TAG
    return 0;
}

static int normalize_is_pure_string_value(const char *input, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (!((c >= '0' && c <= '9') || c == ':' || c == '/' || c == ' ' ||
              c == '.' || c == '%' || c == ',' || c == '+' || c == '-' ||
              c == 'x' || c == '\n')) return 0;
    }
    return 1;
}

static char *normalize_adv_text(const char *input, size_t length, size_t capacity,
                                size_t *output_length) {
    char *output = (char *)malloc(capacity);
    if (!output) return 0;
    size_t used = 0;
    int pure_value = normalize_is_pure_string_value(input, length);
    for (size_t i = 0; i < length;) {
        if (input[i] == ',' && !pure_value) {
            if (used + 3 >= capacity) { free(output); return 0; }
            output[used++] = (char)0xe2; output[used++] = (char)0x80; output[used++] = (char)0x9a;
            ++i;
            continue;
        }
        if (i + 2 < length && (unsigned char)input[i] == 0xe2 &&
            (unsigned char)input[i + 1] == 0xb8 && (unsigned char)input[i + 2] == 0xba) {
            if (used + 3 >= capacity) { free(output); return 0; }
            output[used++] = (char)0xe2; output[used++] = (char)0x80; output[used++] = (char)0x94;
            ++adv_dash_replacements;
            i += 3; continue;
        }
        if (input[i] == '[') {
            size_t end = i + 1;
            while (end < length && input[end] != ']') ++end;
            if (end < length) {
                size_t tag_length = end - i + 1;
                uint32_t previous = utf8_previous_codepoint(input, i);
                int batchim = josa_special_batchim(input, i);
                if (batchim < 0) batchim = josa_batchim(previous);
                const char *replacement = josa_replacement(input + i, tag_length,
                                                            i == 0 ? 1 : batchim > 0, batchim);
                if (replacement) {
                    size_t replacement_length = strlen(replacement);
                    if (used + replacement_length >= capacity) { free(output); return 0; }
                    memcpy(output + used, replacement, replacement_length);
                    used += replacement_length;
                    ++adv_josa_replacements;
                    i = end + 1;
                    continue;
                }
            }
        }
        if (used + 1 >= capacity) { free(output); return 0; }
        output[used++] = input[i++];
    }
    output[used] = 0;
    if (output_length) *output_length = used;
    return output;
}

static char *read_adv_text(const char *name, size_t *output_size) {
    if (!adv_root[0] || !name || !output_size) return 0;
    char path[4608];
    int path_length = snprintf(path, sizeof(path), "%s/%s", adv_root, name);
    if (path_length <= 0 || (size_t)path_length >= sizeof(path)) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return 0;
    }
    long length = ftell(file);
    if (length < 0 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 0;
    }
    size_t capacity = (size_t)length * 3 + 1;
    char *input = (char *)malloc((size_t)length + 1);
    char *output = (char *)malloc(capacity);
    if (!input || !output) {
        if (input) free(input);
        if (output) free(output);
        fclose(file);
        return 0;
    }
    size_t read = fread(input, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(input);
        free(output);
        return 0;
    }
    size_t write = 0;
    for (size_t i = 0; i < read; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == '(' || c == ')') {
            output[write++] = (char)0xef;
            output[write++] = (char)0xbc;
            output[write++] = c == '(' ? (char)0x88 : (char)0x89;
        } else {
            output[write++] = (char)c;
        }
    }
    output[write] = 0;
    free(input);
    *output_size = write;
    return output;
}

/* The Android generic loader keeps three sorted maps.  The iOS package uses
 * the same maps in a compact binary so every TMP/UI setter can perform a
 * bounded lookup without parsing JSON on the Unity thread. */
struct GenericHeader {
    char magic[8];
    uint32_t version, exact, format, split, translated, pool, size;
};
struct GenericEntry { uint32_t key, value, key_len, value_len; };

static const char *generic_string(const struct GenericHeader *header, uint32_t offset) {
    if (!header || header->pool >= header->size || offset >= header->size - header->pool)
        return 0;
    const char *value = (const char *)generic_blob + header->pool + offset;
    const char *end = (const char *)generic_blob + header->size;
    for (const char *p = value; p < end; ++p) if (!*p) return value;
    return 0;
}

static const struct GenericEntry *generic_find_range(const struct GenericHeader *header,
                                                      uint32_t start, uint32_t count,
                                                      const char *key) {
    uint64_t total = header ? (uint64_t)header->exact + header->format + header->split +
                              header->translated : 0;
    if (!header || !key || start > total || count > total - start) return 0;
    const struct GenericEntry *entries = (const struct GenericEntry *)
        ((const char *)generic_blob + sizeof(*header));
    uint32_t low = start, high = start + count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        const char *candidate = generic_string(header, entries[mid].key);
        if (!candidate) return 0;
        int order = strcmp(key, candidate);
        if (!order) return entries + mid;
        if (order < 0) high = mid; else low = mid + 1;
    }
    return 0;
}

static int load_generic_index(void) {
    if (!generic_path[0]) return 0;
    FILE *file = fopen(generic_path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) { if (file) fclose(file); return 0; }
    long length = ftell(file);
    if (length < (long)sizeof(struct GenericHeader) || length > 32 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    void *blob = malloc((size_t)length);
    if (!blob) { fclose(file); return 0; }
    size_t read = fread(blob, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(blob); return 0; }
    const struct GenericHeader *header = (const struct GenericHeader *)blob;
    uint64_t total = (uint64_t)header->exact + header->format + header->split +
                     header->translated;
    uint64_t entries_end = sizeof(*header) + total * sizeof(struct GenericEntry);
    if (memcmp(header->magic, "HSGEN2", 6) || header->version != 2 ||
        header->size != (uint32_t)length || entries_end != header->pool ||
        header->pool >= header->size) { free(blob); return 0; }
    generic_blob = blob;
    generic_blob_size = (size_t)length;
    record("GENERIC INDEX: exact=%u format=%u split=%u translated=%u bytes=%llu",
           header->exact, header->format, header->split, header->translated,
           (unsigned long long)generic_blob_size);
    return 1;
}

static size_t generic_flag_length(const char *text, size_t length, size_t pos) {
    if (pos >= length) return 0;
    unsigned char c = (unsigned char)text[pos];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == ',' || c == '%')
        return 1;
    if (pos + 2 < length && c == 0xef && (unsigned char)text[pos + 1] == 0xbc &&
        ((unsigned char)text[pos + 2] == 0x8b || (unsigned char)text[pos + 2] == 0x8d ||
         (unsigned char)text[pos + 2] == 0x85 || (unsigned char)text[pos + 2] == 0x8c))
        return 3; /* fullwidth +, -, %, comma */
    if (pos + 1 < length && c == 0xc3 && (unsigned char)text[pos + 1] == 0x97)
        return 2; /* multiplication sign */
    return 0;
}

static size_t generic_split_flag_length(const char *text, size_t length, size_t pos) {
    if (pos >= length) return 0;
    unsigned char c = (unsigned char)text[pos];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '%' || c == '.' || c == ':')
        return 1;
    if (pos + 2 < length && c == 0xef && (unsigned char)text[pos + 1] == 0xbc &&
        ((unsigned char)text[pos + 2] == 0x8b || (unsigned char)text[pos + 2] == 0x8d ||
         (unsigned char)text[pos + 2] == 0x85 || (unsigned char)text[pos + 2] == 0x9a))
        return 3; /* fullwidth +, -, %, colon */
    if (pos + 2 < length && c == 0xe3 && (unsigned char)text[pos + 1] == 0x80 &&
        ((unsigned char)text[pos + 2] == 0x90 || (unsigned char)text[pos + 2] == 0x91))
        return 3; /* fullwidth lenticular brackets */
    if (pos + 1 < length && c == 0xc3 && (unsigned char)text[pos + 1] == 0x97)
        return 2; /* multiplication sign */
    return 0;
}

static int generic_append(char *output, size_t capacity, size_t *used,
                           const char *value, size_t length) {
    if (!output || !used || !value || *used + length >= capacity) return 0;
    memcpy(output + *used, value, length);
    *used += length;
    output[*used] = 0;
    return 1;
}

struct GenericFlag { size_t start, length; };

static int generic_formatize(const char *input, size_t length, char *pattern,
                             size_t capacity, struct GenericFlag flags[32],
                             unsigned *flag_count) {
    if (!input || !pattern || !capacity || !flag_count) return 0;
    size_t used = 0, i = 0;
    unsigned count = 0;
    int saw_brace = 0;
    while (i < length) {
        if (input[i] == '{') saw_brace = 1;
        size_t flag_length = generic_flag_length(input, length, i);
        if (flag_length) {
            size_t start = i;
            while (i < length) {
                size_t next = generic_flag_length(input, length, i);
                if (!next) break;
                i += next;
            }
            if (count >= 32 || !generic_append(pattern, capacity, &used, "{", 1)) return 0;
            char number[16];
            int n = snprintf(number, sizeof(number), "%u}", count);
            if (n <= 0 || !generic_append(pattern, capacity, &used, number, (size_t)n)) return 0;
            flags[count].start = start;
            flags[count].length = i - start;
            ++count;
            continue;
        }
        if (!generic_append(pattern, capacity, &used, input + i, 1)) return 0;
        ++i;
    }
    *flag_count = count;
    return count && !saw_brace;
}

static int generic_merge_format(const char *templ, const struct GenericFlag *flags,
                                unsigned flag_count, const char *input, char *output,
                                size_t capacity, size_t *used) {
    size_t i = 0, length = strlen(templ);
    while (i < length) {
        if (templ[i] == '{') {
            size_t p = i + 1;
            unsigned index = 0;
            if (p >= length || templ[p] < '0' || templ[p] > '9') return 0;
            while (p < length && templ[p] >= '0' && templ[p] <= '9') {
                index = index * 10 + (unsigned)(templ[p++] - '0');
                if (index > 31) return 0;
            }
            if (p >= length || templ[p] != '}' || index >= flag_count) return 0;
            if (!generic_append(output, capacity, used, input + flags[index].start,
                                flags[index].length)) return 0;
            i = p + 1;
            continue;
        }
        if (!generic_append(output, capacity, used, templ + i, 1)) return 0;
        ++i;
    }
    return 1;
}

static size_t generic_trim_start(const char *text, size_t start, size_t end) {
    while (start < end && ((unsigned char)text[start] == ' ' ||
                           (unsigned char)text[start] == '\t' ||
                           (unsigned char)text[start] == '\n' ||
                           (unsigned char)text[start] == '\r' ||
                           (unsigned char)text[start] == '\v' ||
                           (unsigned char)text[start] == '\f')) ++start;
    return start;
}

static size_t generic_trim_end(const char *text, size_t start, size_t end) {
    while (end > start && ((unsigned char)text[end - 1] == ' ' ||
                           (unsigned char)text[end - 1] == '\t' ||
                           (unsigned char)text[end - 1] == '\n' ||
                           (unsigned char)text[end - 1] == '\r' ||
                           (unsigned char)text[end - 1] == '\v' ||
                           (unsigned char)text[end - 1] == '\f')) --end;
    return end;
}

static int generic_append_split_chunk(const struct GenericHeader *header, const char *input,
                                      size_t chunk_start, size_t chunk_end, char *output,
                                      size_t capacity, size_t *used) {
    size_t start = generic_trim_start(input, chunk_start, chunk_end);
    size_t end = generic_trim_end(input, start, chunk_end);
    const struct GenericEntry *match = 0;
    char key[512];
    if (start < end && end - start < sizeof(key)) {
        memcpy(key, input + start, end - start);
        key[end - start] = 0;
        match = generic_find_range(header, header->exact + header->format,
                                   header->split, key);
    }
    if (!match)
        return generic_append(output, capacity, used, input + chunk_start,
                              chunk_end - chunk_start) ? 0 : -1;
    const char *translation = generic_string(header, match->value);
    if (!translation ||
        !generic_append(output, capacity, used, input + chunk_start, start - chunk_start) ||
        !generic_append(output, capacity, used, translation, match->value_len) ||
        !generic_append(output, capacity, used, input + end, chunk_end - end)) return -1;
    return 1;
}

static int generic_split_translate(const struct GenericHeader *header, const char *input,
                                   size_t length, char *output, size_t capacity,
                                   size_t *output_length) {
    if (!header || !input || !output || !output_length) return -1;
    int splittable = generic_split_flag_length(input, length, 0) != 0;
    for (size_t p = 0; p < length && !splittable; ++p) {
        if (input[p] == '<' || input[p] == '>' || generic_split_flag_length(input, length, p))
            splittable = 1;
    }
    if (!splittable) return 0;
    size_t used = 0, i = 0, chunk_start = 0;
    int changed = 0;
    while (i < length) {
        size_t flag_length = generic_split_flag_length(input, length, i);
        if (flag_length || input[i] == '<' || input[i] == '>') {
            int chunk = generic_append_split_chunk(header, input, chunk_start, i,
                                                   output, capacity, &used);
            if (chunk < 0) return -1;
            if (chunk > 0) changed = 1;
            size_t separator_length = flag_length ? flag_length : 1;
            if (input[i] == '<') {
                size_t end = i + 1;
                while (end < length && input[end] != '>') ++end;
                if (end < length) separator_length = end - i + 1;
                else separator_length = length - i;
            }
            if (!generic_append(output, capacity, &used, input + i, separator_length)) return -1;
            i += separator_length;
            chunk_start = i;
            continue;
        }
        ++i;
    }
    int chunk = generic_append_split_chunk(header, input, chunk_start, length,
                                           output, capacity, &used);
    if (chunk < 0) return -1;
    if (chunk > 0) changed = 1;
    *output_length = used;
    return changed;
}

static char *generic_translate_utf8(const char *input, size_t length,
                                    size_t *output_length, int *translated,
                                    int normalize_untranslated) {
    if (!input || !output_length) return 0;
    const struct GenericHeader *header = (const struct GenericHeader *)generic_blob;
    size_t capacity = length > (((size_t)-1) - 4097) / 8 ? (size_t)-1 : length * 8 + 4097;
    if (capacity > 4 * 1024 * 1024) capacity = 4 * 1024 * 1024;
    char *selected = (char *)malloc(capacity);
    if (!selected) return 0;
    selected[0] = 0;
    size_t selected_length = 0;
    int did_translate = 0;
    const struct GenericEntry *match = 0;
    if (header && generic_blob_size >= sizeof(*header)) {
        char *key = (char *)malloc(length + 1);
        if (!key) { free(selected); return 0; }
        memcpy(key, input, length); key[length] = 0;
        match = generic_find_range(header, 0, header->exact, key);
        if (match) {
            const char *value = generic_string(header, match->value);
            if (value) { generic_append(selected, capacity, &selected_length, value, match->value_len); did_translate = 1; }
        }
        if (!match) {
            uint32_t translated_start = header->exact + header->format + header->split;
            const struct GenericEntry *known = generic_find_range(
                header, translated_start, header->translated, key);
            if (known) {
                generic_append(selected, capacity, &selected_length, input, length);
                match = (const struct GenericEntry *)1;
            }
        }
        if (!match) {
            size_t base_length = length;
            while (base_length && input[base_length - 1] == '+') --base_length;
            if (base_length < length && base_length) {
                memcpy(key, input, base_length); key[base_length] = 0;
                match = generic_find_range(header, 0, header->exact, key);
                if (match) {
                    const char *value = generic_string(header, match->value);
                    if (value && generic_append(selected, capacity, &selected_length, value,
                                                 match->value_len) &&
                        generic_append(selected, capacity, &selected_length, input + base_length,
                                       length - base_length)) did_translate = 1;
                    else match = 0;
                }
            }
        }
        if (!match) {
            char pattern[4096];
            struct GenericFlag flags[32];
            unsigned flag_count = 0;
            if (generic_formatize(input, length, pattern, sizeof(pattern), flags, &flag_count)) {
                const struct GenericEntry *fmt = generic_find_range(header, header->exact,
                                                                     header->format, pattern);
                const char *templ = fmt ? generic_string(header, fmt->value) : 0;
                if (templ && generic_merge_format(templ, flags, flag_count, input, selected,
                                                  capacity, &selected_length)) {
                    did_translate = 1;
                    match = fmt;
                }
            }
        }
        if (!match) {
            selected_length = 0;
            selected[0] = 0;
            int split_changed = generic_split_translate(header, input, length, selected,
                                                        capacity, &selected_length);
            if (split_changed > 0) { did_translate = 1; match = (const struct GenericEntry *)1; }
            else if (!split_changed) {
                /* Split lookup can write the unchanged input before returning 0.
                 * Replace that tentative output instead of appending it twice. */
                selected_length = 0;
                selected[0] = 0;
                generic_append(selected, capacity, &selected_length, input, length);
            }
            else { free(key); free(selected); return 0; }
        }
        free(key);
    } else {
        generic_append(selected, capacity, &selected_length, input, length);
    }
    if (translated) *translated = did_translate;
    if (!normalize_untranslated && !did_translate) {
        *output_length = selected_length;
        return selected;
    }
    char *normalized = normalize_adv_text(selected, selected_length, capacity, output_length);
    if (normalized) {
        free(selected); return normalized;
    }
    *output_length = selected_length;
    return selected;
}

static void generic_apply_center_layout(void *owner) {
    if (!owner || !font_object_class || !font_method_from_name) return;
    void *klass = font_object_class(owner);
    const void *rich_method = klass ? font_method_from_name(klass, "set_richText", 1) : 0;
    const void *alignment_method = klass ? font_method_from_name(klass, "set_alignment", 1) : 0;
    void *rich_entry = method_entry(rich_method);
    void *alignment_entry = method_entry(alignment_method);
    if (rich_entry) {
        typedef void (*SetBool)(void *, uint8_t, const void *);
        ((SetBool)rich_entry)(owner, 1, rich_method);
    }
    if (alignment_entry) {
        typedef void (*SetInt)(void *, int32_t, const void *);
        ((SetInt)alignment_entry)(owner, 514, alignment_method);
    }
}

static void *localized_managed_string(void *owner, void *value, int normalize_untranslated,
                                      int apply_layout, int *changed, int *translated) {
    if (changed) *changed = 0;
    if (translated) *translated = 0;
    if (!value || !string_length || !string_chars || !string_new_utf8) return value;
    int32_t units = string_length(value);
    if (units < 0 || units > 262144) return value;
    size_t capacity = (size_t)units * 4 + 1;
    char *input = (char *)malloc(capacity);
    if (!input) return value;
    size_t input_length = 0;
    if (!append_managed_utf8(input, capacity, &input_length, value)) {
        free(input); return value;
    }
    int did_translate = 0;
    size_t output_length = 0;
    char *output = generic_translate_utf8(input, input_length, &output_length, &did_translate,
                                          normalize_untranslated);
    if (!output) { free(input); return value; }
    if (apply_layout && output_length >= 8 && !memcmp(output, "[center]", 8)) {
        memmove(output, output + 8, output_length - 8 + 1);
        output_length -= 8;
        generic_apply_center_layout(owner);
    }
    int same = output_length == input_length && !memcmp(input, output, input_length);
    void *result = value;
    if (!same) {
        result = string_new_utf8(output);
        if (result && changed) *changed = 1;
    }
    if (translated) *translated = did_translate;
    free(input);
    free(output);
    return result;
}

static void adv_load_hook(void *self, void *resource_name, void *on_complete,
                          void *on_progress, const void *method) {
    unsigned call = __atomic_add_fetch(&adv_calls, 1, __ATOMIC_RELAXED);
    char name[256];
    if (resource_name_ascii(resource_name, name) && on_complete) {
        size_t byte_length = 0;
        char *text = read_adv_text(name, &byte_length);
        if (text) {
            void *managed = string_new_utf8 ? string_new_utf8(text) : 0;
            free(text);
            void *klass = font_object_class ? font_object_class(on_complete) : 0;
            const void *invoke_method = klass && font_method_from_name
                ? font_method_from_name(klass, "Invoke", 2) : 0;
            void *invoke_entry = 0;
            if (invoke_method) memcpy(&invoke_entry, invoke_method, sizeof(invoke_entry));
            if (managed && invoke_entry) {
                /* Match the working Android port: Action<String,LoadError>.Invoke
                 * is called with the translated string and a null LoadError. */
                typedef void (*Complete)(void *, void *, void *);
                ((Complete)invoke_entry)(on_complete, managed, 0);
                unsigned hit = __atomic_add_fetch(&adv_hits, 1, __ATOMIC_RELAXED);
                if (hit <= 24 || hit == 100)
                    record("ADV TRANSLATED hit=%u name=%s utf8_bytes=%llu", hit, name,
                           (unsigned long long)byte_length);
                return;
            }
            record("ADV WARN: callback resolution failed name=%s managed=%s invoke=%s",
                   name, managed ? "yes" : "no", invoke_entry ? "yes" : "no");
        } else if (call <= 12) {
            record("ADV ORIGINAL call=%u name=%s (no embedded translation)", call, name);
        }
    } else if (call <= 12) {
        record("ADV ORIGINAL call=%u invalid resource or callback", call);
    }
    original_adv_load(self, resource_name, on_complete, on_progress, method);
}

static void record_generic_call(void *original_value, void *localized_value,
                                int did_translate, const char *site) {
    unsigned call = __atomic_add_fetch(&generic_calls, 1, __ATOMIC_RELAXED);
    if (did_translate) {
        unsigned hit = __atomic_add_fetch(&generic_hits, 1, __ATOMIC_RELAXED);
        if (hit <= 24 || hit == 100 || hit == 1000)
            record("GENERIC TRANSLATED hit=%u site=%s", hit, site);
    } else if (call <= 8 && localized_value != original_value) {
        record("GENERIC NORMALIZED site=%s", site);
    }
}

static void tmp_set_text_hook(void *self, void *value, const void *method) {
    int changed = 0, translated = 0;
    void *localized = localized_managed_string(self, value, 1, 1, &changed, &translated);
    record_generic_call(value, localized, translated, "TMP_Text.set_text");
    original_tmp_set_text(self, localized ? localized : value, method);
}

static __attribute__((unused)) void tmp_settext_bool_hook(
        void *self, void *value, uint8_t sync_text_input_box, const void *method) {
    int changed = 0, translated = 0;
    void *localized = localized_managed_string(self, value, 1, 1, &changed, &translated);
    record_generic_call(value, localized, translated, "TMP_Text.SetText(String,bool)");
    original_tmp_settext_bool(self, localized ? localized : value, sync_text_input_box, method);
}

static __attribute__((unused)) void tmp_populate_text_hook(
        void *self, void *value, int32_t start, int32_t length, const void *method) {
    int changed = 0, translated = 0;
    void *localized = value;
    int32_t original_length = value && string_length ? string_length(value) : -1;
    void *source = value;
    if (value && start >= 0 && length >= 0 && start <= original_length &&
        length <= original_length - start && string_new && string_chars) {
        if (start != 0 || length != original_length)
            source = string_new(string_chars(value) + start, length);
        localized = localized_managed_string(self, source, 1, 1, &changed, &translated);
    }
    record_generic_call(value, localized, translated, "TMP_Text.PopulateTextBackingArray");
    if (changed && localized && string_length)
        original_tmp_populate_text(self, localized, 0, string_length(localized), method);
    else
        original_tmp_populate_text(self, value, start, length, method);
}

typedef struct ManagedCharArray {
    void *klass;
    void *monitor;
    void *bounds;
    uintptr_t max_length;
    uint16_t vector[1];
} ManagedCharArray;

static __attribute__((unused)) void tmp_setchararray_hook(
        void *self, void *array, int32_t start, int32_t length, const void *method) {
    int changed = 0, translated = 0;
    void *localized = 0;
    ManagedCharArray *chars = (ManagedCharArray *)array;
    if (chars && chars->max_length <= 262144 && start >= 0 && length >= 0 &&
        (uintptr_t)start <= chars->max_length && (uintptr_t)length <= chars->max_length -
        (uintptr_t)start && string_new) {
        void *source = string_new(chars->vector + start, length);
        localized = localized_managed_string(self, source, 1, 1, &changed, &translated);
        if (localized != source) {
            record_generic_call(source, localized, translated, "TMP_Text.SetCharArray");
            if (original_tmp_set_text) {
                original_tmp_set_text(self, localized, 0);
                return;
            }
        }
    }
    record_generic_call(0, 0, translated, "TMP_Text.SetCharArray");
    original_tmp_setchararray(self, array, start, length, method);
}

static __attribute__((unused)) void textfield_set_value_hook(
        void *self, void *value, const void *method) {
    int changed = 0, translated = 0;
    void *localized = localized_managed_string(self, value, 0, 0, &changed, &translated);
    record_generic_call(value, localized, translated, "TextField.set_value");
    original_textfield_set_value(self, localized ? localized : value, method);
}

static __attribute__((unused)) void ui_text_set_text_hook(
        void *self, void *value, const void *method) {
    int changed = 0, translated = 0;
    void *localized = localized_managed_string(self, value, 1, 0, &changed, &translated);
    record_generic_call(value, localized, translated, "UnityEngine.UI.Text.set_text");
    original_ui_text_set_text(self, localized ? localized : value, method);
}

struct MasterHeader { char magic[8]; uint32_t version, tables, fields, entries, pool, size; };
struct MasterTable { uint32_t name, primary_start, primary_count, local_start,
                                    local_count, entry_start, entry_count; };
struct MasterEntry { uint32_t key, value; };

static const char *master_string(const struct MasterHeader *header, uint32_t offset) {
    if (!header || offset >= header->size - header->pool) return 0;
    const char *value = (const char *)master_blob + header->pool + offset;
    const char *end = (const char *)master_blob + header->size;
    for (const char *p = value; p < end; ++p) if (!*p) return value;
    return 0;
}

static int load_master_index(void) {
    if (!master_path[0]) return 0;
    FILE *file = fopen(master_path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) { if (file) fclose(file); return 0; }
    long length = ftell(file);
    if (length < (long)sizeof(struct MasterHeader) || length > 64 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    void *blob = malloc((size_t)length);
    if (!blob) { fclose(file); return 0; }
    size_t read = fread(blob, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(blob); return 0; }
    const struct MasterHeader *h = (const struct MasterHeader *)blob;
    uint64_t table_end = sizeof(*h) + (uint64_t)h->tables * sizeof(struct MasterTable);
    uint64_t field_end = table_end + (uint64_t)h->fields * sizeof(uint32_t);
    uint64_t entry_end = field_end + (uint64_t)h->entries * sizeof(struct MasterEntry);
    if (memcmp(h->magic, "HSMSTR1", 7) || h->version != 1 || h->size != (uint32_t)length ||
        entry_end != h->pool || h->pool >= h->size) { free(blob); return 0; }
    master_blob = blob;
    master_blob_size = (size_t)length;
    record("MASTER INDEX: tables=%u fields=%u entries=%u bytes=%llu", h->tables,
           h->fields, h->entries, (unsigned long long)master_blob_size);
    return 1;
}

static void *method_entry(const void *method) {
    void *entry = 0;
    if (method) memcpy(&entry, method, sizeof(entry));
    return entry;
}

static int append_managed_utf8(char *output, size_t capacity, size_t *used, void *value) {
    if (!value) return 0;
    int32_t length = string_length(value);
    const uint16_t *chars = length >= 0 ? string_chars(value) : 0;
    if (!chars) return 0;
    for (int32_t i = 0; i < length; ++i) {
        uint32_t c = chars[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < length &&
            chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (chars[++i] - 0xdc00);
        }
        size_t need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (*used + need >= capacity) return 0;
        if (need == 1) output[(*used)++] = (char)c;
        else {
            if (need == 2) output[(*used)++] = (char)(0xc0 | (c >> 6));
            else {
                if (need == 3) output[(*used)++] = (char)(0xe0 | (c >> 12));
                else {
                    output[(*used)++] = (char)(0xf0 | (c >> 18));
                    output[(*used)++] = (char)(0x80 | ((c >> 12) & 0x3f));
                }
                output[(*used)++] = (char)(0x80 | ((c >> 6) & 0x3f));
            }
            output[(*used)++] = (char)(0x80 | (c & 0x3f));
        }
    }
    output[*used] = 0;
    return 1;
}

static int master_prefix(const char *value, const char *prefix, size_t length) {
    if (!value || !prefix) return 0;
    for (size_t i = 0; i < length; ++i) if (value[i] != prefix[i]) return 0;
    return 1;
}

static int master_capitalize(const char *field, size_t length, char *method, size_t capacity,
                             const char *prefix) {
    size_t prefix_length = strlen(prefix);
    if (!field || !method || prefix_length + length + 1 >= capacity) return 0;
    memcpy(method, prefix, prefix_length);
    memcpy(method + prefix_length, field, length);
    method[prefix_length + length] = 0;
    if (length && method[prefix_length] >= 'a' && method[prefix_length] <= 'z')
        method[prefix_length] -= 'a' - 'A';
    return 1;
}

static void *master_getter(void *object, void *klass, const char *field, size_t length) {
    if (!object || !klass || !field || !font_method_from_name) return 0;
    char name[256];
    if (!master_capitalize(field, length, name, sizeof(name), "get_")) return 0;
    const void *method = font_method_from_name(klass, name, 0);
    void *entry = method_entry(method);
    if (!entry) return 0;
    typedef void *(*Getter)(void *, const void *);
    return ((Getter)entry)(object, method);
}

static void *master_list_item(void *list, int32_t index) {
    if (!list || !font_object_class || !font_method_from_name) return 0;
    void *klass = font_object_class(list);
    const void *method = font_method_from_name(klass, "get_Item", 1);
    void *entry = method_entry(method);
    if (!entry) return 0;
    typedef void *(*GetItem)(void *, int32_t, const void *);
    return ((GetItem)entry)(list, index, method);
}

static int master_list_replace_strings(void *list, const char *value) {
    if (!list || !value || !font_object_class || !font_method_from_name || !string_new_utf8)
        return 0;
    void *klass = font_object_class(list);
    const void *clear_method = font_method_from_name(klass, "Clear", 0);
    const void *add_method = font_method_from_name(klass, "Add", 1);
    void *clear_entry = method_entry(clear_method);
    void *add_entry = method_entry(add_method);
    if (!clear_entry || !add_entry) return 0;
    typedef void (*Clear)(void *, const void *);
    typedef void (*Add)(void *, void *, const void *);
    ((Clear)clear_entry)(list, clear_method);
    static const char first_marker[] = "[LA_F]";
    static const char next_marker[] = "[LA_N_F]";
    size_t length = strlen(value), pos = 0;
    int added = 0;
    while (pos < length) {
        if (pos + sizeof(first_marker) - 1 <= length &&
            !memcmp(value + pos, first_marker, sizeof(first_marker) - 1)) {
            pos += sizeof(first_marker) - 1; continue;
        }
        if (pos + sizeof(next_marker) - 1 <= length &&
            !memcmp(value + pos, next_marker, sizeof(next_marker) - 1)) {
            pos += sizeof(next_marker) - 1; continue;
        }
        size_t end = pos;
        while (end < length &&
               !((end + sizeof(first_marker) - 1 <= length &&
                  !memcmp(value + end, first_marker, sizeof(first_marker) - 1)) ||
                 (end + sizeof(next_marker) - 1 <= length &&
                  !memcmp(value + end, next_marker, sizeof(next_marker) - 1)))) ++end;
        if (end > pos) {
            char *part = (char *)malloc(end - pos + 1);
            if (!part) return added;
            memcpy(part, value + pos, end - pos); part[end - pos] = 0;
            void *managed = string_new_utf8(part);
            free(part);
            if (!managed) return added;
            ((Add)add_entry)(list, managed, add_method);
            ++added;
        }
        pos = end;
    }
    return added > 0;
}

static int master_apply_path(void *message, void *message_class, const char *path,
                             const char *translation) {
    if (!message || !message_class || !path || !translation) return 0;
    void *current = message;
    void *current_class = message_class;
    size_t pos = 0, path_length = strlen(path);
    while (pos < path_length) {
        size_t name_start = pos;
        while (pos < path_length && path[pos] != '.' && path[pos] != '[') ++pos;
        size_t name_length = pos - name_start;
        if (!name_length || name_length >= 256) return 0;
        int32_t index = -1;
        if (pos < path_length && path[pos] == '[') {
            ++pos;
            int value = 0;
            int digits = 0;
            while (pos < path_length && path[pos] >= '0' && path[pos] <= '9') {
                value = value * 10 + path[pos++] - '0';
                if (value > 100000) return 0;
                digits = 1;
            }
            if (!digits || pos >= path_length || path[pos++] != ']') return 0;
            index = (int32_t)value;
        }
        int final = pos >= path_length;
        if (!final && path[pos++] != '.') return 0;
        if (final) {
            void *property = master_getter(current, current_class, path + name_start, name_length);
            if (index >= 0) {
                void *list = property;
                void *item = master_list_item(list, index);
                if (!item) return 0;
                property = item;
            }
            /* Tutorial.stepInfo[].texts is a List<String> with no scalar
             * setter.  Android clears and repopulates that list. */
            if (index < 0 && property && master_list_replace_strings(property, translation)) return 1;
            if (!string_new_utf8 || !font_method_from_name) return 0;
            char setter[256];
            if (!master_capitalize(path + name_start, name_length, setter, sizeof(setter), "set_"))
                return 0;
            const void *method = font_method_from_name(current_class, setter, 1);
            void *entry = method_entry(method);
            void *managed = entry ? string_new_utf8(translation) : 0;
            if (!entry || !managed) return 0;
            typedef void (*Setter)(void *, void *, const void *);
            ((Setter)entry)(current, managed, method);
            return 1;
        }
        void *next = master_getter(current, current_class, path + name_start, name_length);
        if (!next) return 0;
        if (index >= 0) next = master_list_item(next, index);
        if (!next || !font_object_class) return 0;
        current = next;
        current_class = font_object_class(next);
    }
    return 0;
}

static void localize_master(void *message) {
    if (!message || !master_blob || !class_get_name || !font_object_class ||
        !font_method_from_name) return;
    const struct MasterHeader *h = (const struct MasterHeader *)master_blob;
    const char *base = (const char *)master_blob;
    const struct MasterTable *tables = (const struct MasterTable *)(base + sizeof(*h));
    const uint32_t *fields = (const uint32_t *)(tables + h->tables);
    const struct MasterEntry *entries = (const struct MasterEntry *)(fields + h->fields);
    void *klass = font_object_class(message);
    const char *class_name = klass ? class_get_name(klass) : 0;
    if (!class_name) return;
    const struct MasterTable *table = 0;
    for (uint32_t i = 0; i < h->tables; ++i) {
        const char *name = master_string(h, tables[i].name);
        if (name && !strcmp(name, class_name)) { table = tables + i; break; }
    }
    if (!table || table->primary_start + table->primary_count > h->fields ||
        table->local_start + table->local_count > h->fields ||
        table->entry_start + table->entry_count > h->entries) return;
    char prefix[2048];
    size_t prefix_length = 0;
    for (uint32_t i = 0; i < table->primary_count; ++i) {
        const char *field = master_string(h, fields[table->primary_start + i]);
        if (!field || prefix_length + strlen(field) + 2 >= sizeof(prefix)) return;
        char getter[256];
        size_t field_length = strlen(field);
        if (!master_capitalize(field, field_length, getter, sizeof(getter), "get_")) return;
        const void *get_method = font_method_from_name(klass, getter, 0);
        void *get_entry = method_entry(get_method);
        if (!get_entry) return;
        typedef void *(*Getter)(void *, const void *);
        void *value = ((Getter)get_entry)(message, get_method);
        uintptr_t scalar = (uintptr_t)value;
        if (scalar <= 0xffffffffULL) {
            int count = snprintf(prefix + prefix_length, sizeof(prefix) - prefix_length,
                                 "%d", (int32_t)scalar);
            if (count <= 0 || (size_t)count >= sizeof(prefix) - prefix_length) return;
            prefix_length += (size_t)count;
        } else if (!append_managed_utf8(prefix, sizeof(prefix), &prefix_length, value)) return;
        if (prefix_length + 1 >= sizeof(prefix)) return;
        prefix[prefix_length++] = '|'; prefix[prefix_length] = 0;
    }
    unsigned applied = 0;
    uint32_t entry_end = table->entry_start + table->entry_count;
    for (uint32_t i = table->entry_start; i < entry_end; ++i) {
        const char *key = master_string(h, entries[i].key);
        const char *translation = master_string(h, entries[i].value);
        if (!key || !translation || strlen(key) <= prefix_length ||
            !master_prefix(key, prefix, prefix_length)) continue;
        const char *path = key + prefix_length;
        char normalized[512];
        size_t used = 0;
        for (size_t p = 0; path[p] && used + 1 < sizeof(normalized); ++p) {
            if (path[p] == '[') {
                while (path[p] && path[p] != ']') ++p;
                continue;
            }
            normalized[used++] = path[p];
        }
        normalized[used] = 0;
        int known = 0;
        for (uint32_t f = 0; f < table->local_count; ++f) {
            const char *local = master_string(h, fields[table->local_start + f]);
            if (local && !strcmp(local, normalized)) { known = 1; break; }
        }
        if (!known) continue;
        if (master_apply_path(message, klass, path, translation)) ++applied;
    }
    if (applied) {
        unsigned hit = __atomic_add_fetch(&master_hits, applied, __ATOMIC_RELAXED);
        if (hit <= 100) record("MASTER TRANSLATED table=%s fields=%u total=%u", class_name, applied, hit);
    }
}

static void master_merge_hook(void *message, void *span, void *method) {
    original_master_merge(message, span, method);
    __atomic_add_fetch(&master_calls, 1, __ATOMIC_RELAXED);
    localize_master(message);
}


#endif

static void image_added(const struct mach_header *header, intptr_t slide) {
    (void)slide;
    if (armed) return;
    Dl_info info;
    if (!dladdr(header, &info) || !info.dli_fname ||
        !strstr(info.dli_fname, "/UnityFramework.framework/UnityFramework")) return;
    uintptr_t base = (uintptr_t)header;
#ifndef HOSHIMI_TEXT_ONLY
#endif
#ifdef HOSHIMI_DISCOVERY
    unity_base = base;
#endif
#ifndef HOSHIMI_TEXT_ONLY
    unity_handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    const char *framework_marker = strstr(info.dli_fname, "/Frameworks/UnityFramework.framework/UnityFramework");
    if (framework_marker) {
        size_t prefix_length = (size_t)(framework_marker - info.dli_fname);
        if (prefix_length < 2048)
            snprintf(adv_root, sizeof(adv_root), "%.*s/HoshimiLocal/local-files/resource/adv",
                     (int)prefix_length, info.dli_fname);
        if (prefix_length < 2048)
            snprintf(master_path, sizeof(master_path), "%.*s/HoshimiLocal/master.bin",
                     (int)prefix_length, info.dli_fname);
        if (prefix_length < 2048)
            snprintf(generic_path, sizeof(generic_path), "%.*s/HoshimiLocal/generic.bin",
                     (int)prefix_length, info.dli_fname);
    }
#endif
    if (memcmp((void *)(base + HOSHIMI_TARGET_RVA), patched_entry_hex, sizeof(patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_CAVE_RVA), gateway_hex, sizeof(gateway_hex))) {
        record("FAIL: static branch/gateway mismatch; hook not armed");
        return;
    }
#ifndef HOSHIMI_TEXT_ONLY
    if (memcmp((void *)(base + HOSHIMI_FONT_TARGET_RVA), font_patched_entry_hex,
               sizeof(font_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_FONT_CAVE_RVA), font_gateway_hex,
               sizeof(font_gateway_hex))) {
        record("FAIL: TMP_FontAsset.Awake branch/gateway mismatch; hook not armed");
        return;
    }
    if (memcmp((void *)(base + HOSHIMI_ADV_TARGET_RVA), adv_patched_entry_hex,
               sizeof(adv_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_ADV_CAVE_RVA), adv_gateway_hex,
               sizeof(adv_gateway_hex))) {
        record("FAIL: ADV branch/gateway mismatch; hook not armed");
        return;
    }
    if (memcmp((void *)(base + HOSHIMI_MASTER_TARGET_RVA), master_patched_entry_hex,
               sizeof(master_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_MASTER_CAVE_RVA), master_gateway_hex,
               sizeof(master_gateway_hex))) {
        record("FAIL: MasterDB branch/gateway mismatch; hook not armed");
        return;
    }
    if (memcmp((void *)(base + HOSHIMI_TMP_SET_TEXT_TARGET_RVA), tmp_set_text_patched_entry_hex, sizeof(tmp_set_text_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_TMP_SET_TEXT_CAVE_RVA), tmp_set_text_gateway_hex, sizeof(tmp_set_text_gateway_hex))) {
        record("FAIL: tmp_set_text static gateway mismatch"); return;
    }
    if (memcmp((void *)(base + HOSHIMI_TMP_POPULATE_TARGET_RVA), tmp_populate_patched_entry_hex, sizeof(tmp_populate_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_TMP_POPULATE_CAVE_RVA), tmp_populate_gateway_hex, sizeof(tmp_populate_gateway_hex))) {
        record("FAIL: tmp_populate static gateway mismatch"); return;
    }
    if (memcmp((void *)(base + HOSHIMI_TMP_SETTEXT_BOOL_TARGET_RVA), tmp_settext_bool_patched_entry_hex, sizeof(tmp_settext_bool_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_TMP_SETTEXT_BOOL_CAVE_RVA), tmp_settext_bool_gateway_hex, sizeof(tmp_settext_bool_gateway_hex))) {
        record("FAIL: tmp_settext_bool static gateway mismatch"); return;
    }
    if (memcmp((void *)(base + HOSHIMI_TMP_SETCHARARRAY_TARGET_RVA), tmp_setchararray_patched_entry_hex, sizeof(tmp_setchararray_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_TMP_SETCHARARRAY_CAVE_RVA), tmp_setchararray_gateway_hex, sizeof(tmp_setchararray_gateway_hex))) {
        record("FAIL: tmp_setchararray static gateway mismatch"); return;
    }
    if (memcmp((void *)(base + HOSHIMI_TEXTFIELD_TARGET_RVA), textfield_patched_entry_hex, sizeof(textfield_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_TEXTFIELD_CAVE_RVA), textfield_gateway_hex, sizeof(textfield_gateway_hex))) {
        record("FAIL: textfield static gateway mismatch"); return;
    }
    if (memcmp((void *)(base + HOSHIMI_UI_TEXT_TARGET_RVA), ui_text_patched_entry_hex, sizeof(ui_text_patched_entry_hex)) ||
        memcmp((void *)(base + HOSHIMI_UI_TEXT_CAVE_RVA), ui_text_gateway_hex, sizeof(ui_text_gateway_hex))) {
        record("FAIL: ui_text static gateway mismatch"); return;
    }

#endif
    original = (SetValue)(base + HOSHIMI_ORIGINAL_RVA);
    string_length = (StringLength)(base + API_IL2CPP_STRING_LENGTH);
    string_chars = (StringChars)(base + API_IL2CPP_STRING_CHARS);
    string_new = (StringNew)(base + API_IL2CPP_STRING_NEW_UTF16);
    string_new_utf8 = (StringNewUtf8)(base + API_IL2CPP_STRING_NEW);
#ifndef HOSHIMI_TEXT_ONLY
    original_font_awake = (FontAwake)(base + HOSHIMI_FONT_ORIGINAL_RVA);
    original_adv_load = (AdvLoad)(base + HOSHIMI_ADV_ORIGINAL_RVA);
    original_master_merge = (MasterMerge)(base + HOSHIMI_MASTER_ORIGINAL_RVA);
    font_get_name = (FontGetName)(base + 0x7843a14ULL);
    font_set_multi_atlas = (FontSetBool)(base + 0x771cd08ULL);
    font_get_fallback_table = (FontGetFallbackTable)(base + 0x771cd98ULL);
    font_clear_data = (FontSetBool)(base + 0x7724914ULL);
    font_object_class = (FontObjectClass)dlsym(unity_handle, "il2cpp_object_get_class");
    font_method_from_name = (FontMethodFromName)dlsym(unity_handle, "il2cpp_class_get_method_from_name");
    class_get_name = (ClassGetName)dlsym(unity_handle, "il2cpp_class_get_name");
    if (!font_object_class || !font_method_from_name || !class_get_name) {
        record("FAIL: font list resolver exports missing");
        return;
    }
    uintptr_t *font_slot = (uintptr_t *)(base + HOSHIMI_FONT_SLOT_RVA);
    uintptr_t font_expected = 0;
    if (!__atomic_compare_exchange_n(font_slot, &font_expected, (uintptr_t)font_awake_hook, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        record("FAIL: reserved font hook slot is already occupied");
        return;
    }
    uintptr_t *adv_slot = (uintptr_t *)(base + HOSHIMI_ADV_SLOT_RVA);
    uintptr_t adv_expected = 0;
    if (!__atomic_compare_exchange_n(adv_slot, &adv_expected, (uintptr_t)adv_load_hook, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        record("FAIL: reserved ADV hook slot is already occupied");
        return;
    }
    if (master_enabled && load_master_index()) {
        uintptr_t *master_slot = (uintptr_t *)(base + HOSHIMI_MASTER_SLOT_RVA);
        uintptr_t master_expected = 0;
        if (!__atomic_compare_exchange_n(master_slot, &master_expected,
                                         (uintptr_t)master_merge_hook, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            record("FAIL: reserved MasterDB hook slot is already occupied");
            return;
        }
        record("ARMED Google.Protobuf.MessageExtensions.MergeFrom rva=0x%llx",
               (unsigned long long)HOSHIMI_MASTER_TARGET_RVA);
    } else if (master_enabled) {
        record("MASTER DISABLED: embedded master.bin was not found or invalid");
    } else {
        record("MASTER DISABLED: useMasterTrans=OFF");
    }
    if (load_generic_index()) {
        record("GENERIC READY: exact/translated/plus/format/split lookup active");
    } else {
        record("GENERIC WARN: embedded generic.bin was not found or invalid; ligature/josa normalization remains active");
    }
    original_tmp_set_text = (TmpSetText)(base + HOSHIMI_TMP_SET_TEXT_ORIGINAL_RVA);
    original_tmp_populate_text = (TmpPopulateText)(base + HOSHIMI_TMP_POPULATE_ORIGINAL_RVA);
    original_tmp_settext_bool = (TmpSetTextBool)(base + HOSHIMI_TMP_SETTEXT_BOOL_ORIGINAL_RVA);
    original_tmp_setchararray = (TmpSetCharArray)(base + HOSHIMI_TMP_SETCHARARRAY_ORIGINAL_RVA);
    original_textfield_set_value = (UiSetText)(base + HOSHIMI_TEXTFIELD_ORIGINAL_RVA);
    original_ui_text_set_text = (UiSetText)(base + HOSHIMI_UI_TEXT_ORIGINAL_RVA);
    __atomic_store_n((uintptr_t *)(base + HOSHIMI_TMP_SET_TEXT_SLOT_RVA), (uintptr_t)tmp_set_text_hook, __ATOMIC_RELEASE);
    __atomic_store_n((uintptr_t *)(base + HOSHIMI_TMP_POPULATE_SLOT_RVA), (uintptr_t)tmp_populate_text_hook, __ATOMIC_RELEASE);
    __atomic_store_n((uintptr_t *)(base + HOSHIMI_TMP_SETTEXT_BOOL_SLOT_RVA), (uintptr_t)tmp_settext_bool_hook, __ATOMIC_RELEASE);
    __atomic_store_n((uintptr_t *)(base + HOSHIMI_TMP_SETCHARARRAY_SLOT_RVA), (uintptr_t)tmp_setchararray_hook, __ATOMIC_RELEASE);
    __atomic_store_n((uintptr_t *)(base + HOSHIMI_TEXTFIELD_SLOT_RVA), (uintptr_t)textfield_set_value_hook, __ATOMIC_RELEASE);
    __atomic_store_n((uintptr_t *)(base + HOSHIMI_UI_TEXT_SLOT_RVA), (uintptr_t)ui_text_set_text_hook, __ATOMIC_RELEASE);
    record("GENERIC ARMED: 6 static text hooks; no runtime code patching");
#endif
    uintptr_t *slot = (uintptr_t *)(base + HOSHIMI_SLOT_RVA);
    uintptr_t expected = 0;
    if (!__atomic_compare_exchange_n(slot, &expected, (uintptr_t)set_value_hook, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        record("FAIL: reserved data slot is already occupied");
        return;
    }
    armed = 1;
    record("ARMED Qua.UI.I18n.SetValue rva=0x%llx entries=%u; waiting for calls",
           (unsigned long long)HOSHIMI_TARGET_RVA, TRANSLATION_COUNT);
#ifndef HOSHIMI_TEXT_ONLY
    record("ARMED TMPro.TMP_FontAsset.Awake rva=0x%llx; Android-style font capture active",
           (unsigned long long)HOSHIMI_FONT_TARGET_RVA);
    record("ARMED OctoResourceLoader.LoadFromCacheOrDownload rva=0x%llx; ADV root=%s",
           (unsigned long long)HOSHIMI_ADV_TARGET_RVA, adv_root[0] ? adv_root : "<unavailable>");
#ifdef HOSHIMI_DISCOVERY
    if (!metadata_probe_started) {
        metadata_probe_started = 1;
        pthread_t worker;
        if (pthread_create(&worker, 0, probe_translation_targets, 0) == 0) {
            pthread_detach(worker);
            record("DISCOVERY START: generic/master/resource target lookup running");
        } else {
            record("DISCOVERY FAIL: worker creation failed");
        }
    }
#endif
#endif
}

__attribute__((constructor)) static void start(void) {
    const char *home = getenv("HOME");
    char path[4096];
    int n = home ? snprintf(path, sizeof(path), "%s/Documents/hoshimi-ios-hook.log", home) : -1;
    if (n > 0 && (size_t)n < sizeof(path)) log_file = fopen(path, "w");
#ifdef HOSHIMI_TEXT_ONLY
    record("Hoshimi iOS hook v8: translation-only isolation build");
    record("Font activation disabled; Korean glyphs are expected to render as squares");
#else
    record("Hoshimi iOS hook v29: static text hooks; generic duplication fixed");
    record("Static SourceSansPro-Regular OTF replacement expected in sharedassets0.assets");
#endif
    patch_enabled = read_boolean_setting("HoshimiLocalifyEnabled", 1);
    diagnostics_enabled = read_boolean_setting("HoshimiDiagnosticsEnabled", 0);
    master_enabled = read_boolean_setting("useMasterTrans", 1);
    images_enabled = read_boolean_setting("replaceImages", 1);
    phone_subtitles_enabled = read_boolean_setting("usePhoneSubtitles", 1);
    record("PATCH SETTING: HoshimiLocalifyEnabled=%s", patch_enabled ? "ON" : "OFF");
    record("DIAGNOSTICS SETTING: HoshimiDiagnosticsEnabled=%s",
           diagnostics_enabled ? "ON" : "OFF");
    record("FEATURE SETTINGS: useMasterTrans=%s replaceImages=%s usePhoneSubtitles=%s",
           master_enabled ? "ON" : "OFF", images_enabled ? "ON" : "OFF",
           phone_subtitles_enabled ? "ON" : "OFF");
    if (diagnostics_enabled) {
        record("PATCH SUSPENDED: isolated diagnostics dylib will collect addresses");
        return;
    }
    if (!patch_enabled) {
        record("PATCH DISABLED: restart after changing Settings > Apps > 아이프라");
        return;
    }
    _dyld_register_func_for_add_image(image_added);
}
