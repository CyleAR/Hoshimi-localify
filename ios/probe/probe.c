/* Read-only IL2CPP address inventory for the iOS translation port.
 * This diagnostic never installs a hook or invokes a managed game method. */
#include "darwin_abi.h"

static const char *unity_path;
static FILE *log_file;

static int diagnostics_enabled(void) {
    typedef void *(*StringCreate)(void *, const char *, uint32_t);
    typedef uint8_t (*GetBoolean)(void *, void *, uint8_t *);
    typedef void (*Release)(void *);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!cf) return 0;
    StringCreate make_string = (StringCreate)dlsym(cf, "CFStringCreateWithCString");
    GetBoolean get_boolean = (GetBoolean)dlsym(cf, "CFPreferencesGetAppBooleanValue");
    Release release = (Release)dlsym(cf, "CFRelease");
    void **current_application = (void **)dlsym(cf, "kCFPreferencesCurrentApplication");
    if (!make_string || !get_boolean || !release || !current_application || !*current_application) {
        dlclose(cf);
        return 0;
    }
    void *key = make_string(0, "HoshimiDiagnosticsEnabled", 0x08000100u);
    if (!key) { dlclose(cf); return 0; }
    uint8_t exists = 0;
    uint8_t value = get_boolean(key, *current_application, &exists);
    release(key);
    dlclose(cf);
    return exists && value != 0;
}

struct Target {
    const char *image;
    const char *namespaze;
    const char *klass;
    const char *methods;
};

static const struct Target targets[] = {
    {"quaunity-ui.Runtime.dll", "Qua.UI", "I18nHelper", "SetValue|SetUpI18n"},
    {"quaunity-ui.Runtime.dll", "Qua.UI", "I18n", "SetValue|GetOrDefault"},
    {"Assembly-CSharp.dll", "Solis.OutGame", "ADVEnginePresenter", "get_UserName"},
    {"Assembly-CSharp.dll", "Solis.Common.Proto.Master", "MessageDetail", "GetReplacedMessage|GetNotificationText"},
    {"Unity.TextMeshPro.dll", "TMPro", "TMP_FontAsset", "Awake|set_isMultiAtlasTexturesEnabled|get_fallbackFontAssetTable|ClearFontAssetData"},
    {"Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_text|SetText|PopulateTextBackingArray|SetCharArray|get_font|set_font|SetAllDirty|SetVerticesDirty|SetMaterialDirty"},
    {"Unity.TextMeshPro.dll", "TMPro", "TextMeshProUGUI", "Awake"},
    {"UnityEngine.UIElementsModule.dll", "UnityEngine.UIElements", "TextField", "set_value"},
    {"UnityEngine.UI.dll", "UnityEngine.UI", "Text", "set_text"},
    {"UnityEngine.UI.dll", "UnityEngine.UI", "Image", "set_sprite|set_overrideSprite"},
    {"UnityEngine.UI.dll", "UnityEngine.UI", "RawImage", "set_texture"},
    {"UnityEngine.UI.dll", "UnityEngine.UI", "Graphic", "OnEnable"},
    {"Google.Protobuf.dll", "Google.Protobuf", "MessageExtensions", "MergeFrom"},
    {"Octo.dll", "Octo.Caching", "OctoCaching", "GetResourceFileName"},
    {"Octo.dll", "Octo.Loader", "OctoResourceLoader", "LoadFromCacheOrDownload"},
    {"Octo.dll", "Octo", "OnDownloadProgress", "Invoke"},
    {"UnityEngine.AssetBundleModule.dll", "UnityEngine", "AssetBundle", "LoadAsset"},
    {"UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "Play|PlayDelayed|PlayOneShot|set_clip"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Object", "get_name"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Component", "get_transform"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "get_parent"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Time", "get_realtimeSinceStartup"},
};

static const char *icalls[] = {
    "UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)",
    "UnityEngine.AssetBundleRequest::GetResult()",
    "UnityEngine.ResourcesAPIInternal::Load(System.String,System.Type)",
};

static void record(const char *format, ...) {
    if (!log_file) return;
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);
}

static int method_wanted(const char *list, const char *name) {
    if (!list || !name) return 0;
    size_t length = strlen(name);
    const char *at = list;
    while (*at) {
        const char *end = at;
        while (*end && *end != '|') ++end;
        if ((size_t)(end - at) == length && !memcmp(at, name, length)) return 1;
        at = *end ? end + 1 : end;
    }
    return 0;
}

static void image_added(const struct mach_header *header, intptr_t slide) {
    (void)slide;
    Dl_info info;
    if (dladdr(header, &info) && info.dli_fname &&
        strstr(info.dli_fname, "/UnityFramework.framework/UnityFramework"))
        __atomic_store_n(&unity_path, info.dli_fname, __ATOMIC_RELEASE);
}

static void *probe(void *unused) {
    (void)unused;
    char path[4096];
    const char *home = getenv("HOME");
    if (!home) return 0;
    int n = snprintf(path, sizeof(path), "%s/Documents/hoshimi-ios-probe.log", home);
    if (n <= 0 || (size_t)n >= sizeof(path)) return 0;
    log_file = fopen(path, "w");
    if (!log_file) return 0;
    record("Hoshimi iOS probe v4: full translation hook address inventory");
    record("Expected input: IDOLY PRIDE 6.0.2 build 141");
    record("MODE: diagnostics only; no translation/font/image hook installed");

    void *handle = 0;
    for (int attempt = 0; attempt < 180; ++attempt) {
        const char *image = __atomic_load_n(&unity_path, __ATOMIC_ACQUIRE);
        if (image) {
            handle = dlopen(image, RTLD_NOW | RTLD_NOLOAD);
            if (handle) break;
        }
        sleep(1);
    }
    if (!handle) { record("FAIL: UnityFramework unavailable within 180 seconds"); goto done; }

#define RESOLVE(type, var, symbol) \
    type var = (type)dlsym(handle, symbol); \
    if (!var) { record("FAIL: missing export %s", symbol); goto done; }
    typedef void *(*NoArgPointer)(void);
    typedef const void **(*Assemblies)(void *, size_t *);
    typedef void *(*PointerArg)(void *);
    typedef void (*VoidPointerArg)(void *);
    typedef const char *(*Name)(const void *);
    typedef void *(*ClassFromName)(const void *, const char *, const char *);
    typedef const void *(*Methods)(void *, void **);
    typedef uint32_t (*ParamCount)(const void *);
    typedef const void *(*Param)(const void *, uint32_t);
    typedef char *(*TypeName)(const void *);
    typedef const void *(*FieldFromName)(void *, const char *);
    typedef int32_t (*FieldOffset)(const void *);
    typedef void *(*ResolveIcall)(const char *);
    RESOLVE(NoArgPointer, domain_get, "il2cpp_domain_get");
    RESOLVE(Assemblies, assemblies_get, "il2cpp_domain_get_assemblies");
    RESOLVE(PointerArg, thread_attach, "il2cpp_thread_attach");
    RESOLVE(VoidPointerArg, thread_detach, "il2cpp_thread_detach");
    RESOLVE(PointerArg, image_get, "il2cpp_assembly_get_image");
    RESOLVE(Name, image_name, "il2cpp_image_get_name");
    RESOLVE(ClassFromName, class_get, "il2cpp_class_from_name");
    RESOLVE(Methods, methods_get, "il2cpp_class_get_methods");
    RESOLVE(Name, method_name, "il2cpp_method_get_name");
    RESOLVE(ParamCount, param_count, "il2cpp_method_get_param_count");
    RESOLVE(Param, param_get, "il2cpp_method_get_param");
    RESOLVE(TypeName, type_name, "il2cpp_type_get_name");
    RESOLVE(VoidPointerArg, il2cpp_free, "il2cpp_free");
    RESOLVE(FieldFromName, field_get, "il2cpp_class_get_field_from_name");
    RESOLVE(FieldOffset, field_offset, "il2cpp_field_get_offset");
    RESOLVE(ResolveIcall, resolve_icall, "il2cpp_resolve_icall");
    record("UnityFramework loaded; all resolver exports present");

    for (int attempt = 0; attempt < 180; ++attempt) {
        void *domain = domain_get();
        size_t assembly_count = 0;
        const void **assemblies = domain ? assemblies_get(domain, &assembly_count) : 0;
        if (!assemblies || !assembly_count) { sleep(1); continue; }
        void *thread = thread_attach(domain);
        if (!thread) { record("FAIL: thread_attach returned null"); goto done; }
        unsigned classes_found = 0, methods_found = 0;
        for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
            void *image = 0;
            for (size_t i = 0; i < assembly_count; ++i) {
                void *candidate = image_get((void *)assemblies[i]);
                const char *candidate_name = candidate ? image_name(candidate) : 0;
                if (candidate_name && !strcmp(candidate_name, targets[t].image)) { image = candidate; break; }
            }
            void *klass = image ? class_get(image, targets[t].namespaze, targets[t].klass) : 0;
            if (!klass) {
                record("CLASS MISS %s %s.%s", targets[t].image, targets[t].namespaze, targets[t].klass);
                continue;
            }
            ++classes_found;
            if (!strcmp(targets[t].klass, "TMP_FontAsset")) {
                const void *field = field_get(klass, "m_FaceInfo");
                record("FIELD TMPro.TMP_FontAsset.m_FaceInfo offset=0x%x",
                       field ? (unsigned)field_offset(field) : 0xffffffffu);
            }
            unsigned class_methods = 0;
            void *iter = 0;
            const void *method = 0;
            while ((method = methods_get(klass, &iter))) {
                const char *name = method_name(method);
                if (!method_wanted(targets[t].methods, name)) continue;
                void *entry = 0;
                memcpy(&entry, method, sizeof(entry));
                Dl_info info;
                if (!entry || !dladdr(entry, &info) || !info.dli_fname ||
                    !strstr(info.dli_fname, "/UnityFramework.framework/UnityFramework")) {
                    record("TARGET INVALID %s.%s", targets[t].klass, name ? name : "<null>");
                    continue;
                }
                uint32_t count = param_count(method);
                record("TARGET %s::%s.%s params=%u rva=0x%llx", targets[t].namespaze,
                       targets[t].klass, name, count,
                       (unsigned long long)((uintptr_t)entry - (uintptr_t)info.dli_fbase));
                for (uint32_t p = 0; p < count; ++p) {
                    char *type = type_name(param_get(method, p));
                    record("  parameter[%u]=%s", p, type ? type : "<unknown>");
                    if (type) il2cpp_free(type);
                }
                ++class_methods;
                ++methods_found;
            }
            if (!class_methods)
                record("METHOD MISS %s.%s wanted=%s", targets[t].namespaze, targets[t].klass, targets[t].methods);
        }
        for (size_t i = 0; i < sizeof(icalls) / sizeof(icalls[0]); ++i) {
            void *entry = resolve_icall(icalls[i]);
            Dl_info info;
            if (entry && dladdr(entry, &info) && info.dli_fname &&
                strstr(info.dli_fname, "/UnityFramework.framework/UnityFramework"))
                record("ICALL %s rva=0x%llx", icalls[i],
                       (unsigned long long)((uintptr_t)entry - (uintptr_t)info.dli_fbase));
            else
                record("ICALL MISS %s", icalls[i]);
        }
        thread_detach(thread);
        if (classes_found >= 18 && methods_found >= 24) {
            record("PASS: classes=%u methods=%u; full inventory completed", classes_found, methods_found);
            goto done;
        }
        record("RETRY: classes=%u methods=%u", classes_found, methods_found);
        sleep(1);
    }
    record("FAIL: full inventory did not reach completeness threshold");
done:
    if (handle) dlclose(handle);
    fclose(log_file);
    log_file = 0;
    return 0;
}

__attribute__((constructor)) static void hoshimi_start(void) {
    if (!diagnostics_enabled()) return;
    _dyld_register_func_for_add_image(image_added);
    pthread_t worker;
    if (pthread_create(&worker, 0, probe, 0) == 0) pthread_detach(worker);
}
