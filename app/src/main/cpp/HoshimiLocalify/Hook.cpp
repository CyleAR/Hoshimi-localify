#include "shadowhook.h"
#include <android/log.h>
#include "Hook.h"
#include "Plugin.h"
#include "Log.h"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "Il2cppUtils.hpp"
#include "Local.h"
#include "MasterLocal.h"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include "camera/camera.hpp"
#include "config/Config.hpp"
// #include <jni.h>
#include <thread>
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>

std::unordered_set<void*> hookedStubs{};
extern std::filesystem::path hoshimiLocalPath;


#define DEFINE_HOOK(returnType, name, params)                                                      \
	using name##_Type = returnType(*) params;                                                      \
	name##_Type name##_Addr = nullptr;                                                             \
	name##_Type name##_Orig = nullptr;                                                             \
	returnType name##_Hook params

#define ADD_HOOK(name, addr)                                                                       \
	name##_Addr = reinterpret_cast<name##_Type>(addr);                                             \
	if (addr) {                                                                                    \
    	auto stub = hookInstaller->InstallHook(reinterpret_cast<void*>(addr),                      \
                                               reinterpret_cast<void*>(name##_Hook),               \
                                               reinterpret_cast<void**>(&name##_Orig));            \
        if (stub == NULL) {                                                                        \
            int error_num = shadowhook_get_errno();                                                \
            const char *error_msg = shadowhook_to_errmsg(error_num);                               \
            Log::ErrorFmt("ADD_HOOK: %s at %p failed: %s", #name, addr, error_msg);                \
        }                                                                                          \
        else {                                                                                     \
            hookedStubs.emplace(stub);                                                             \
            HoshimiLocal::Log::InfoFmt("ADD_HOOK: %s at %p", #name, addr);                         \
        }                                                                                          \
    }                                                                                              \
    else HoshimiLocal::Log::ErrorFmt("Hook failed: %s is NULL", #name, addr);                      \
    if (Config::lazyInit) UnityResolveProgress::classProgress.current++

/*
void UnHookAll() {
    for (const auto i: hookedStubs) {
        int result = shadowhook_unhook(i);
        if(result != 0)
        {
            int error_num = shadowhook_get_errno();
            const char *error_msg = shadowhook_to_errmsg(error_num);
            HoshimiLocal::Log::ErrorFmt("unhook failed: %d - %s", error_num, error_msg);
        }
    }
}*/

namespace HoshimiLocal::HookMain {
    using Il2cppString = UnityResolve::UnityType::String;

    void LogImageResourceDebug(const char* source, void* component, const std::string& assetName, bool replaced) {
        if (!Config::debugImageResourceLog || assetName.empty()) return;
        Log::DebugFmt("ResourceDebug[%s]: asset=%s replaced=%d component=%p",
                      source, assetName.c_str(), replaced ? 1 : 0, component);
    }

    using LiveUtility_LoadLiveResult_Type = void(*)(void* data, void* method);
    LiveUtility_LoadLiveResult_Type LiveUtility_LoadLiveResult_Call = nullptr;

    UnityResolve::UnityType::String* environment_get_stacktrace() {
        /*
        static auto mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System",
                                                 "Environment", "get_StackTrace");
        return mtd->Invoke<UnityResolve::UnityType::String*>();*/
        const auto pClass = Il2cppUtils::GetClass("mscorlib.dll", "System.Diagnostics",
                                                  "StackTrace");

        const auto ctor_mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System.Diagnostics",
                                                     "StackTrace", ".ctor");
        const auto toString_mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System.Diagnostics",
                                                         "StackTrace", "ToString");

        const auto klassInstance = pClass->New<void*>();
        ctor_mtd->Invoke<void>(klassInstance);
        return toString_mtd->Invoke<Il2cppString*>(klassInstance);
    }

    DEFINE_HOOK(void, Internal_LogException, (void* ex, void* obj)) {
        Internal_LogException_Orig(ex, obj);
        if (!ex) return;
        static auto Exception_ToString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
        if (!Exception_ToString) return;
        Log::LogUnityLog(ANDROID_LOG_ERROR, "UnityLog - Internal_LogException:\n%s", Exception_ToString->Invoke<Il2cppString*>(ex)->ToString().c_str());
    }

    DEFINE_HOOK(void, Internal_Log, (int logType, int logOption, UnityResolve::UnityType::String* content, void* context)) {
        Internal_Log_Orig(logType, logOption, content, context);
        if (!content) return;
        // 2022.3.21f1
        Log::LogUnityLog(ANDROID_LOG_VERBOSE, "Internal_Log:\n%s", content->ToString().c_str());
    }

    bool IsNativeObjectAlive(void* obj) {
        if (!obj) return false;
        static UnityResolve::Method* IsNativeObjectAliveMtd = nullptr;
        if (!IsNativeObjectAliveMtd) IsNativeObjectAliveMtd = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                                     "Object", "IsNativeObjectAlive");
        if (!IsNativeObjectAliveMtd) return false;
        return IsNativeObjectAliveMtd->Invoke<bool>(obj);
    }

    UnityResolve::UnityType::Camera* mainCameraCache = nullptr;
    UnityResolve::UnityType::Transform* cameraTransformCache = nullptr;
    void CheckAndUpdateMainCamera() {
        if (!Config::enableFreeCamera) return;
        if (IsNativeObjectAlive(mainCameraCache) && IsNativeObjectAlive(cameraTransformCache)) return;

        mainCameraCache = UnityResolve::UnityType::Camera::GetMain();
        cameraTransformCache = mainCameraCache->GetTransform();
    }

    Il2cppUtils::Resolution_t GetResolution() {
        static auto GetResolution = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine",
                                                           "Screen", "get_currentResolution");
        return GetResolution->Invoke<Il2cppUtils::Resolution_t>();
    }

    Il2cppString* ToJsonStr(void* object) {
        static Il2cppString* (*toJsonStr)(void*) = nullptr;
		if (!toJsonStr) {
			toJsonStr = reinterpret_cast<Il2cppString * (*)(void*)>(Il2cppUtils::GetMethodPointer("Newtonsoft.Json.dll", "Newtonsoft.Json",
                "JsonConvert", "SerializeObject", { "*" }));
        }
        if (!toJsonStr) {
			return nullptr;
        }
		return toJsonStr(object);
    }

    DEFINE_HOOK(void, Unity_set_fieldOfView, (UnityResolve::UnityType::Camera* self, float value)) {
        if (Config::enableFreeCamera) {
            if (self == mainCameraCache) {
                value = IPCamera::baseCamera.fov;
            }
        }
        Unity_set_fieldOfView_Orig(self, value);
    }

    DEFINE_HOOK(float, Unity_get_fieldOfView, (UnityResolve::UnityType::Camera* self)) {
        if (Config::enableFreeCamera) {
            if (self == mainCameraCache) {
                static auto get_orthographic = reinterpret_cast<bool (*)(void*)>(Il2cppUtils::il2cpp_resolve_icall(
                        "UnityEngine.Camera::get_orthographic()"
                ));
                static auto set_orthographic = reinterpret_cast<bool (*)(void*, bool)>(Il2cppUtils::il2cpp_resolve_icall(
                        "UnityEngine.Camera::set_orthographic(System.Boolean)"
                ));

                for (const auto& i : UnityResolve::UnityType::Camera::GetAllCamera()) {
                    // Log::DebugFmt("get_orthographic: %d", get_orthographic(i));
                    // set_orthographic(i, false);
                    Unity_set_fieldOfView_Orig(i, IPCamera::baseCamera.fov);
                }
                Unity_set_fieldOfView_Orig(self, IPCamera::baseCamera.fov);

                // Log::DebugFmt("main - get_orthographic: %d", get_orthographic(self));
                return IPCamera::baseCamera.fov;
            }
        }
        return Unity_get_fieldOfView_Orig(self);
    }

    UnityResolve::UnityType::Transform* cacheTrans = nullptr;
    UnityResolve::UnityType::Quaternion cacheRotation{};
    UnityResolve::UnityType::Vector3 cachePosition{};
    UnityResolve::UnityType::Vector3 cacheForward{};
    UnityResolve::UnityType::Vector3 cacheLookAt{};

    DEFINE_HOOK(void, Unity_set_rotation_Injected, (UnityResolve::UnityType::Transform* self, UnityResolve::UnityType::Quaternion* value)) {
        if (Config::enableFreeCamera) {
            static auto lookat_injected = reinterpret_cast<void (*)(void*self,
                                                                    UnityResolve::UnityType::Vector3* worldPosition, UnityResolve::UnityType::Vector3* worldUp)>(
                    Il2cppUtils::il2cpp_resolve_icall(
                            "UnityEngine.Transform::Internal_LookAt_Injected(UnityEngine.Vector3&,UnityEngine.Vector3&)"));
            static auto worldUp = UnityResolve::UnityType::Vector3(0, 1, 0);

            if (cameraTransformCache == self) {
                const auto cameraMode = IPCamera::GetCameraMode();
                if (cameraMode == IPCamera::CameraMode::FIRST_PERSON) {
                    if (cacheTrans && IsNativeObjectAlive(cacheTrans)) {
                        if (IPCamera::GetFirstPersonRoll() == IPCamera::FirstPersonRoll::ENABLE_ROLL) {
                            *value = cacheRotation;
                        }
                        else {
                            static HoshimiLocal::Misc::FixedSizeQueue<float> recordsY(60);
                            const auto newY = IPCamera::CheckNewY(cacheLookAt, true, recordsY);
                            UnityResolve::UnityType::Vector3 newCacheLookAt{cacheLookAt.x, newY, cacheLookAt.z};
                            lookat_injected(self, &newCacheLookAt, &worldUp);
                            return;
                        }
                    }
                }
                else if (cameraMode == IPCamera::CameraMode::FOLLOW) {
                    auto newLookAtPos = IPCamera::CalcFollowModeLookAt(cachePosition,
                                                                       IPCamera::followPosOffset, true);
                    lookat_injected(self, &newLookAtPos, &worldUp);
                    return;
                }
                else {
                    auto& origCameraLookat = IPCamera::baseCamera.lookAt;
                    lookat_injected(self, &origCameraLookat, &worldUp);
                    // Log::DebugFmt("fov: %f, target: %f", Unity_get_fieldOfView_Orig(mainCameraCache), IPCamera::baseCamera.fov);
                    return;
                }
            }
        }
        return Unity_set_rotation_Injected_Orig(self, value);
    }

    DEFINE_HOOK(void, Unity_set_position_Injected, (UnityResolve::UnityType::Transform* self, UnityResolve::UnityType::Vector3* data)) {
        if (Config::enableFreeCamera) {
            CheckAndUpdateMainCamera();

            if (cameraTransformCache == self) {
                const auto cameraMode = IPCamera::GetCameraMode();
                if (cameraMode == IPCamera::CameraMode::FIRST_PERSON) {
                    if (cacheTrans && IsNativeObjectAlive(cacheTrans)) {
                        *data = IPCamera::CalcFirstPersonPosition(cachePosition, cacheForward, IPCamera::firstPersonPosOffset);
                    }

                }
                else if (cameraMode == IPCamera::CameraMode::FOLLOW) {
                    auto newLookAtPos = IPCamera::CalcFollowModeLookAt(cachePosition, IPCamera::followPosOffset);
                    auto pos = IPCamera::CalcPositionFromLookAt(newLookAtPos, IPCamera::followPosOffset);
                    data->x = pos.x;
                    data->y = pos.y;
                    data->z = pos.z;
                }
                else {
                    //Log::DebugFmt("MainCamera set pos: %f, %f, %f", data->x, data->y, data->z);
                    auto& origCameraPos = IPCamera::baseCamera.pos;
                    data->x = origCameraPos.x;
                    data->y = origCameraPos.y;
                    data->z = origCameraPos.z;
                }
            }
        }

        return Unity_set_position_Injected_Orig(self, data);
    }

    DEFINE_HOOK(void*, InternalSetOrientationAsync, (void* self, int type, void* c, void* tc, void* mtd)) {
        switch (Config::gameOrientation) {
        case 1: type = 0x2; break;  // FixedPortrait
        case 2: type = 0x3; break;  // FixedLandscape
        default: break;
        }
        return InternalSetOrientationAsync_Orig(self, type, c, tc, mtd);
    }

    DEFINE_HOOK(void, EndCameraRendering, (void* ctx, void* camera, void* method)) {
        EndCameraRendering_Orig(ctx, camera, method);

        if (Config::enableFreeCamera && mainCameraCache) {
            Unity_set_fieldOfView_Orig(mainCameraCache, IPCamera::baseCamera.fov);
            if (IPCamera::GetCameraMode() == IPCamera::CameraMode::FIRST_PERSON) {
                mainCameraCache->SetNearClipPlane(0.001f);
            }
        }
    }

    DEFINE_HOOK(void, Unity_set_targetFrameRate, (int value)) {
        const auto configFps = Config::targetFrameRate;
        return Unity_set_targetFrameRate_Orig(configFps == 0 ? value: configFps);
    }

    std::unordered_map<void*, std::string> loadHistory{};

    DEFINE_HOOK(void*, AssetBundle_LoadAssetAsync, (void* self, Il2cppString* name, void* type)) {
        if (Config::debugImageResourceLog && name) {
            Log::DebugFmt("ResourceDebug[AssetBundle.LoadAssetAsync]: asset=%s", name->ToString().c_str());
        }
        auto ret = AssetBundle_LoadAssetAsync_Orig(self, name, type);
        if (ret && name) loadHistory.emplace(ret, name->ToString());
        return ret;
    }

    DEFINE_HOOK(void*, AssetBundleRequest_GetResult, (void* self)) {
        auto result = AssetBundleRequest_GetResult_Orig(self);
        if (const auto iter = loadHistory.find(self); iter != loadHistory.end()) {
            const auto name = iter->second;
            loadHistory.erase(iter);

            if (Config::debugImageResourceLog) {
                const auto assetClass = Il2cppUtils::get_class_from_instance(result);
                const char* typeName = assetClass ? static_cast<Il2cppUtils::Il2CppClassHead*>(assetClass)->name : "<null>";
                Log::DebugFmt("ResourceDebug[AssetBundleRequest.GetResult]: asset=%s type=%s",
                              name.c_str(), typeName);
            }
        }
        return result;
    }

    DEFINE_HOOK(void*, Resources_Load, (Il2cppString* path, void* systemTypeInstance)) {
        auto ret = Resources_Load_Orig(path, systemTypeInstance);
        if (Config::debugImageResourceLog && path) {
            Log::DebugFmt("ResourceDebug[Resources.Load]: path=%s", path->ToString().c_str());
        }
        return ret;
    }

    DEFINE_HOOK(void, I18nHelper_SetUpI18n, (void* self, Il2cppString* lang, Il2cppString* localizationText, int keyComparison)) {
        std::string locTextStr = localizationText->ToString();
        Log::InfoFmt("SetUpI18n lang: %s, parsing %zu bytes of csv...", lang->ToString().c_str(), locTextStr.length());
        
        nlohmann::ordered_json fullDumpJson;
        
        std::stringstream ss(locTextStr);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line.substr(0, 2) == "//") {
                continue;
            }
            size_t commaPos = line.find(',');
            if (commaPos != std::string::npos) {
                std::string key = line.substr(0, commaPos);
                std::string value = line.substr(commaPos + 1);
                fullDumpJson[key] = value;
            }
        }
        
        try {
            auto fullDumpPath = Local::GetBasePath() / "dump-files" / "localization.json";
            std::ofstream out(fullDumpPath);
            out << fullDumpJson.dump(4, 32, false);
            out.close();
            Log::InfoFmt("SetUpI18n full dump saved to %s", fullDumpPath.string().c_str());
        } catch (std::exception& e) {
            Log::ErrorFmt("Failed to save full dump: %s", e.what());
        }
        
        Log::InfoFmt("SetUpI18n CSV parsed and dumped.");
        I18nHelper_SetUpI18n_Orig(self, lang, localizationText, keyComparison);
    }

    DEFINE_HOOK(void, I18nHelper_SetValue, (void* self, Il2cppString* key, Il2cppString* value)) {
        // Log::InfoFmt("I18nHelper_SetValue: %s - %s", key->ToString().c_str(), value->ToString().c_str());
        std::string local;
        if (Local::GetI18n(key->ToString(), &local)) {
            I18nHelper_SetValue_Orig(self, key, UnityResolve::UnityType::String::New(local));
            return;
        }
        if (Config::textTest) {
            I18nHelper_SetValue_Orig(self, key, Il2cppString::New("[I18]" + value->ToString()));
        }
        else {
            I18nHelper_SetValue_Orig(self, key, value);
        }
    }

    void* koPatchedFontAsset = nullptr;
    void* runtimeKoreanFontAsset = nullptr;
    void* runtimeKoreanFontBundle = nullptr;
    bool runtimeKoreanFontLoadTried = false;
    bool fallbackRegistered = false;
    void* solisFontAsset = nullptr;
    void* sourceSansProAsset = nullptr;
    std::vector<void*> observedFontAssets{};

    void MarkReplacementAssetPersistent(void* obj);
    void InjectKoreanFallbackIntoObservedFonts(const char* source);
    void* GetKoreanFontAsset();

    std::string GetUnityObjectName(void* obj) {
        if (!obj) return "<null>";
        static auto get_name = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Object", "get_name");
        if (!get_name) return "<no get_name>";
        auto nameStr = get_name->Invoke<Il2cppString*>(obj);
        return nameStr ? nameStr->ToString() : "<null name>";
    }

    struct MaterialColor {
        float r;
        float g;
        float b;
        float a;
    };

    void CopyMaterialFloatProperty(void* srcMaterial, void* dstMaterial, const char* propName) {
        static auto HasProperty = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                         "UnityEngine", "Material", "HasProperty",
                                                         {"System.String"});
        static auto GetFloat = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Material", "GetFloat",
                                                      {"System.String"});
        static auto SetFloat = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Material", "SetFloat",
                                                      {"System.String", "System.Single"});
        if (!srcMaterial || !dstMaterial || !HasProperty || !GetFloat || !SetFloat) return;
        auto propStr = Il2cppString::New(propName);
        if (!HasProperty->Invoke<bool>(srcMaterial, propStr) ||
            !HasProperty->Invoke<bool>(dstMaterial, propStr)) {
            return;
        }
        SetFloat->Invoke<void>(dstMaterial, propStr, GetFloat->Invoke<float>(srcMaterial, propStr));
    }

    void CopyMaterialColorProperty(void* srcMaterial, void* dstMaterial, const char* propName) {
        static auto HasProperty = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                         "UnityEngine", "Material", "HasProperty",
                                                         {"System.String"});
        static auto GetColor = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Material", "GetColor",
                                                      {"System.String"});
        static auto SetColor = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Material", "SetColor",
                                                      {"System.String", "UnityEngine.Color"});
        if (!srcMaterial || !dstMaterial || !HasProperty || !GetColor || !SetColor) return;
        auto propStr = Il2cppString::New(propName);
        if (!HasProperty->Invoke<bool>(srcMaterial, propStr) ||
            !HasProperty->Invoke<bool>(dstMaterial, propStr)) {
            return;
        }
        SetColor->Invoke<void>(dstMaterial, propStr, GetColor->Invoke<MaterialColor>(srcMaterial, propStr));
    }

    void CopyRuntimeFontMaterialStyle(void* srcMaterial, void* dstMaterial) {
        if (!srcMaterial || !dstMaterial || srcMaterial == dstMaterial) return;

        // Keep atlas-bound properties on the runtime font material:
        // _MainTex, _TextureWidth, _TextureHeight, _GradientScale, _ScaleRatioA/B/C.
        const char* floatProps[] = {
                "_FaceDilate", "_OutlineWidth", "_OutlineSoftness",
                "_WeightNormal", "_WeightBold",
                "_Bevel", "_BevelOffset", "_BevelWidth", "_BevelClamp", "_BevelRoundness",
                "_LightAngle", "_SpecularPower", "_Reflectivity", "_Diffuse", "_Ambient",
                "_BumpOutline", "_BumpFace",
                "_GlowOffset", "_GlowInner", "_GlowOuter", "_GlowPower",
                "_UnderlayOffsetX", "_UnderlayOffsetY", "_UnderlayDilate", "_UnderlaySoftness",
                "_VertexOffsetX", "_VertexOffsetY",
                "_MaskSoftnessX", "_MaskSoftnessY",
                "_StencilComp", "_Stencil", "_StencilOp", "_StencilWriteMask", "_StencilReadMask", "_ColorMask"
        };
        for (const auto prop : floatProps) {
            CopyMaterialFloatProperty(srcMaterial, dstMaterial, prop);
        }

        const char* colorProps[] = {
                "_FaceColor", "_OutlineColor", "_SpecularColor", "_ReflectFaceColor",
                "_ReflectOutlineColor", "_GlowColor", "_UnderlayColor", "_ClipRect"
        };
        for (const auto prop : colorProps) {
            CopyMaterialColorProperty(srcMaterial, dstMaterial, prop);
        }

    }

    void ActivateKoreanFont() {
        if (Config::useRuntimeKoreanFont) return;
        if (koPatchedFontAsset) return;
        if (!sourceSansProAsset || !solisFontAsset) return;

        static auto set_isMultiAtlasTexturesEnabled = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                             "TMP_FontAsset", "set_isMultiAtlasTexturesEnabled");
        static auto ClearFontAssetData = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                "TMP_FontAsset", "ClearFontAssetData");
        static auto TryAddCharacters = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                              "TMP_FontAsset", "TryAddCharacters",
                                                              {"System.String", "System.Boolean"});

        // Solis-MK5 SDF의 FaceInfo 메트릭(pointSize/scale 등)을 SourceSansPro에 복사
        // → 한글 크기가 일본어와 동일해짐
        static int faceInfoOffset = -1;
        if (faceInfoOffset < 0) {
            static auto klass = Il2cppUtils::GetClass("Unity.TextMeshPro.dll", "TMPro", "TMP_FontAsset");
            if (klass) {
                auto field = klass->Get<UnityResolve::Field>("m_FaceInfo");
                if (field) faceInfoOffset = field->offset;
            }
        }
        
        if (faceInfoOffset > 0) {
            // 문자열 ref 16바이트 건너뛰고 숫자 필드 68바이트만 복사
            constexpr int strSize  = 16;
            constexpr int numSize  = 68;
            auto* src = (char*)solisFontAsset + faceInfoOffset + strSize;
            auto* dst = (char*)sourceSansProAsset + faceInfoOffset + strSize;
            std::memcpy(dst, src, numSize);
            Log::InfoFmt("[Font] Matched Korean font size to Solis via FaceInfo memcpy");
        }

        if (set_isMultiAtlasTexturesEnabled)
            set_isMultiAtlasTexturesEnabled->Invoke<void>(sourceSansProAsset, reinterpret_cast<void*>(1));
        if (ClearFontAssetData)
            ClearFontAssetData->Invoke<void>(sourceSansProAsset, reinterpret_cast<void*>(0));
        
        koPatchedFontAsset = sourceSansProAsset;
        fallbackRegistered = false;
        Log::InfoFmt("[Font] Korean font activation complete (SourceSansPro-Regular SDF)");
    }

    void* GetKoreanFontAsset() {
        if (Config::useRuntimeKoreanFont) {
            if (runtimeKoreanFontAsset && IsNativeObjectAlive(runtimeKoreanFontAsset)) {
                return runtimeKoreanFontAsset;
            }
            if (runtimeKoreanFontLoadTried) return nullptr;
            runtimeKoreanFontLoadTried = true;

            const auto bundlePath = Local::GetBasePath() / "local-files" / "koreanfont";
            std::ifstream file(bundlePath, std::ios::binary);
            if (!file.is_open()) {
                Log::ErrorFmt("[Font] Runtime Korean font bundle not found: %s", bundlePath.string().c_str());
                return nullptr;
            }

            file.seekg(0, std::ios::end);
            const auto fileSize = file.tellg();
            if (fileSize <= 0) {
                Log::ErrorFmt("[Font] Runtime Korean font bundle is empty: %s", bundlePath.string().c_str());
                return nullptr;
            }
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
            file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
            if (!file.good()) {
                Log::ErrorFmt("[Font] Failed to read runtime Korean font bundle: %s", bundlePath.string().c_str());
                return nullptr;
            }

            static auto byteKlass = Il2cppUtils::GetClass("mscorlib.dll", "System", "Byte");
            if (!byteKlass) return nullptr;
            auto il2cppBytes = UnityResolve::UnityType::Array<uint8_t>::New(byteKlass, bytes.size());
            std::memcpy(reinterpret_cast<void*>(il2cppBytes->GetData()), bytes.data(), bytes.size());

            using LoadFromMemoryInternal_t = void* (*)(void*, uint32_t);
            static auto LoadFromMemory_Internal = reinterpret_cast<LoadFromMemoryInternal_t>(
                    Il2cppUtils::il2cpp_resolve_icall("UnityEngine.AssetBundle::LoadFromMemory_Internal(System.Byte[],System.UInt32)"));
            if (!LoadFromMemory_Internal) {
                LoadFromMemory_Internal = reinterpret_cast<LoadFromMemoryInternal_t>(
                        Il2cppUtils::il2cpp_resolve_icall("UnityEngine.AssetBundle::LoadFromMemory_Internal(System.Byte[])"));
            }
            if (!LoadFromMemory_Internal) {
                Log::ErrorFmt("[Font] AssetBundle.LoadFromMemory_Internal icall not found");
                return nullptr;
            }

            runtimeKoreanFontBundle = LoadFromMemory_Internal(il2cppBytes, 0);
            if (!runtimeKoreanFontBundle) {
                Log::ErrorFmt("[Font] AssetBundle.LoadFromMemory_Internal returned null");
                return nullptr;
            }
            MarkReplacementAssetPersistent(runtimeKoreanFontBundle);

            using LoadAssetInternal_t = void* (*)(void*, void*, void*);
            static auto LoadAsset_Internal = reinterpret_cast<LoadAssetInternal_t>(
                    Il2cppUtils::il2cpp_resolve_icall("UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)"));
            if (!LoadAsset_Internal) {
                Log::ErrorFmt("[Font] AssetBundle.LoadAsset_Internal icall not found");
                return nullptr;
            }

            static auto tmpFontKlass = Il2cppUtils::GetClass("Unity.TextMeshPro.dll", "TMPro", "TMP_FontAsset");
            static auto unityObjectKlass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
            void* assetType = tmpFontKlass ? tmpFontKlass->GetType() : (unityObjectKlass ? unityObjectKlass->GetType() : nullptr);
            if (!assetType) {
                Log::ErrorFmt("[Font] TMP_FontAsset/Object System.Type not available");
                return nullptr;
            }

            const char* assetNames[] = {
                    "PretendardJP-SemiBold SDF",
                    "PretendardJP-SemiBold SDF.asset",
                    "PretendardJP-SemiBold",
                    "koreanfont"
            };
            for (const auto name : assetNames) {
                auto asset = LoadAsset_Internal(runtimeKoreanFontBundle, Il2cppString::New(name), assetType);
                if (!asset) continue;
                runtimeKoreanFontAsset = asset;
                MarkReplacementAssetPersistent(runtimeKoreanFontAsset);
                fallbackRegistered = false;
                Log::InfoFmt("[Font] Runtime Korean TMP_FontAsset loaded from bundle: %s", name);
                InjectKoreanFallbackIntoObservedFonts("RuntimeKoreanFontBundle.LoadAsset");
                return runtimeKoreanFontAsset;
            }

            Log::ErrorFmt("[Font] TMP_FontAsset was not found in runtime Korean font bundle");
            return nullptr;
        }

        if (koPatchedFontAsset && IsNativeObjectAlive(koPatchedFontAsset)) {
            return koPatchedFontAsset;
        }
        // Awake 훅이 아직 발동 안 된 경우: 로그 스팩 방지
        static bool warnedOnce = false;
        if (!warnedOnce) {
            warnedOnce = true;
            Log::ErrorFmt("[Font] Korean font asset not ready (SourceSansPro-Regular SDF not loaded yet)");
        }
        return nullptr;
    }

    bool ListContains(void* list, void* item) {
        if (!list || !item) return false;
        Il2cppUtils::Tools::CSListEditor<void*> editor(list);
        const auto count = editor.get_Count();
        if (count <= 0) return false;
        for (int i = 0; i < count; ++i) {
            if (editor.get_Item(i) == item) return true;
        }
        return false;
    }

    bool InsertFontAssetFallback(void* list, void* fontAsset, const char* target, bool first) {
        if (!list || !fontAsset) return false;
        if (ListContains(list, fontAsset)) return true;

        static auto List_Insert = Il2cppUtils::GetMethod("mscorlib.dll", "System.Collections.Generic", "List`1", "Insert");
        static auto List_Add = Il2cppUtils::GetMethod("mscorlib.dll", "System.Collections.Generic", "List`1", "Add");
        if (first && List_Insert) {
            List_Insert->Invoke<void>(list, 0, fontAsset);
        } else if (List_Add) {
            List_Add->Invoke<void>(list, fontAsset);
        } else if (List_Insert) {
            List_Insert->Invoke<void>(list, 0, fontAsset);
        } else {
            Log::ErrorFmt("[Font] List.Insert/Add not found for %s", target);
            return false;
        }

        return true;
    }

    bool InsertKoreanFontFirst(void* list, const char* target) {
        auto koFont = GetKoreanFontAsset();
        if (!list || !koFont) return false;
        return InsertFontAssetFallback(list, koFont, target, true);
    }

    void RegisterKoreanFontFallback() {
        if (fallbackRegistered) return;
        auto koFont = GetKoreanFontAsset();
        if (!koFont) return;

        static auto get_fallbackFontAssets = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                    "TMP_Settings", "get_fallbackFontAssets");
        if (!get_fallbackFontAssets) {
            Log::ErrorFmt("[Font] TMP_Settings.get_fallbackFontAssets not found!");
            return;
        }
        auto fallbackList = get_fallbackFontAssets->Invoke<void*>(nullptr);
        if (!fallbackList) {
            Log::ErrorFmt("[Font] TMP_Settings fallbackFontAssets list is null!");
            return;
        }
        if (InsertKoreanFontFirst(fallbackList, "TMP_Settings.fallbackFontAssets")) {
            fallbackRegistered = true;
            return;
        }
        static auto List_Add = Il2cppUtils::GetMethod("mscorlib.dll", "System.Collections.Generic",
                                                      "List`1", "Add");
        if (List_Add) {
            List_Add->Invoke<void>(fallbackList, koFont);
            fallbackRegistered = true;
            Log::InfoFmt("[Font] Korean font registered in TMP_Settings.fallbackFontAssets!");
        }
    }
 
    std::string ResolveJosa(std::string text) {
        if (text.empty()) return text;

        auto get_batchim = [](uint32_t code) -> int {
            if (code < 0xAC00 || code > 0xD7A3) return -1;
            return (code - 0xAC00) % 28;
        };

        size_t pos = 0;
        while ((pos = text.find("[", pos)) != std::string::npos) {
            size_t endPos = text.find("]", pos);
            if (endPos == std::string::npos) break;

            std::string tag = text.substr(pos, endPos - pos + 1);
            std::string replaceWith = "";
            
            // 앞 글자 유니코드 추출 (UTF-8 백트래킹)
            uint32_t lastChar = 0;
            if (pos > 0) {
                size_t prev = pos - 1;
                if ((text[prev] & 0x80) == 0) { // ASCII
                    lastChar = text[prev];
                } else { // Multi-byte
                    while (prev > 0 && (text[prev] & 0xC0) == 0x80) prev--;
                    // Simple UTF-8 to UTF-32 (3-byte Hangeul focus)
                    unsigned char c1 = text[prev];
                    if ((c1 & 0xF0) == 0xE0 && prev + 2 < pos) {
                        lastChar = ((c1 & 0x0F) << 12) | ((text[prev+1] & 0x3F) << 6) | (text[prev+2] & 0x3F);
                    }
                }
            }

            int batchim = get_batchim(lastChar);
            bool hasBatchim = (pos == 0) ? true : (batchim > 0);

            if (tag == "[은/는]") replaceWith = hasBatchim ? "\xEC\x9D\x80" : "\xEB\x8A\x94"; // 은 : 는
            else if (tag == "[이/가]") replaceWith = hasBatchim ? "\xEC\x9D\xB4" : "\xEA\xB0\x80"; // 이 : 가
            else if (tag == "[이/랑]" || tag == "[이랑/랑]") replaceWith = hasBatchim ? "\xEC\x9D\xB4\xEB\x9E\x91" : "\xEB\x9E\x91"; // 이랑 : 랑
            else if (tag == "[이/라]" || tag == "[이라/라]") replaceWith = hasBatchim ? "\xEC\x9D\xB4\xEB\x9D\xBC" : "\xEB\x9D\xBC"; // 이라 : 라
            else if (tag == "[을/를]") replaceWith = hasBatchim ? "\xEC\x9D\x84" : "\xEB\xA5\xBC"; // 을 : 를
            else if (tag == "[와/과]") replaceWith = hasBatchim ? "\xEA\xB3\xBC" : "\xEC\x99\x80"; // 과 : 와
            else if (tag == "[아/야]") replaceWith = hasBatchim ? "\xEC\x95\x84" : "\xEC\x95\xBC"; // 아 : 야
            else if (tag == "[으/로]" || tag == "[으로/로]") {
                // ㄹ 받침(8)인 경우 '로' 선택
                if (batchim == 8) replaceWith = "\xEB\xA1\x9C"; // 로
                else replaceWith = hasBatchim ? "\xEC\x9C\xBC\xEB\xA1\x9C" : "\xEB\xA1\x9C"; // 으로 : 로
            }

            if (!replaceWith.empty()) {
                text.replace(pos, tag.length(), replaceWith);
                pos += replaceWith.length();
            } else {
                pos = endPos + 1;
            }
        }
        return text;
    }

    std::string FixLigature(std::string text) {
        if (text.empty()) return text;
        size_t pos = 0;
        // 1. 스토리창 범인: U+2E3A (Two-Em Dash, \xE2\xB8\xBA) 를 Em Dash(—) 하나로 치환
        while ((pos = text.find("\xE2\xB8\xBA", pos)) != std::string::npos) {
            text.replace(pos, 3, "\xE2\x80\x94");
            pos += 3;
        }

        // 2. 한국어 조사 처리 추가
        text = ResolveJosa(text);

        return text;
    }

    std::unordered_set<void*> updatedFontPtrs{};
    std::unordered_set<void*> forcedTextPtrs{};
    std::unordered_set<void*> strippedKoreanLookupFontPtrs{};
    std::unordered_set<void*> runtimeFallbackOriginalFontPtrs{};

    void TrackObservedFontAsset(void* fontAsset) {
        if (!fontAsset) return;
        if (std::find(observedFontAssets.begin(), observedFontAssets.end(), fontAsset) != observedFontAssets.end()) return;
        if (observedFontAssets.size() > 512) observedFontAssets.erase(observedFontAssets.begin());
        observedFontAssets.emplace_back(fontAsset);
    }

    void StripKoreanCharactersFromFontAsset(void* fontAsset) {
        auto koFont = GetKoreanFontAsset();
        if (!fontAsset || !koFont || fontAsset == koFont) return;
        if (strippedKoreanLookupFontPtrs.contains(fontAsset)) return;
        if (strippedKoreanLookupFontPtrs.size() > 512) strippedKoreanLookupFontPtrs.clear();

        static auto tmpFontKlass = Il2cppUtils::GetClass("Unity.TextMeshPro.dll", "TMPro", "TMP_FontAsset");
        static int characterLookupOffset = -1;
        if (characterLookupOffset < 0 && tmpFontKlass) {
            auto field = tmpFontKlass->Get<UnityResolve::Field>("m_CharacterLookupDictionary");
            if (field) characterLookupOffset = field->offset;
        }
        if (characterLookupOffset <= 0) return;

        auto characterLookup = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(fontAsset) + characterLookupOffset);
        if (!characterLookup) return;

        static auto Dictionary_Remove = Il2cppUtils::GetMethod("mscorlib.dll", "System.Collections.Generic", "Dictionary`2", "Remove");
        if (!Dictionary_Remove) return;

        int removed = 0;
        auto removeRange = [&](uint32_t begin, uint32_t end) {
            for (uint32_t code = begin; code <= end; ++code) {
                if (Dictionary_Remove->Invoke<bool>(characterLookup, code)) {
                    removed++;
                }
            }
        };

        removeRange(0x1100, 0x11FF);
        removeRange(0x3130, 0x318F);
        removeRange(0xA960, 0xA97F);
        removeRange(0xAC00, 0xD7A3);
        removeRange(0xD7B0, 0xD7FF);

        strippedKoreanLookupFontPtrs.emplace(fontAsset);
        if (removed > 0) {
            Log::InfoFmt("[Font] Removed %d Korean character lookups from primary font to force fallback", removed);
        }
    }

    void InjectKoreanFallbackIntoFontAsset(void* fontAsset, const char* source) {
        if (!Config::replaceFont || !fontAsset) return;
        auto koFont = GetKoreanFontAsset();
        if (!koFont || fontAsset == koFont) return;

        static auto get_fallbackFontAssetTable = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                        "TMP_FontAsset", "get_fallbackFontAssetTable");
        static auto set_fallbackFontAssetTable = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                        "TMP_FontAsset", "set_fallbackFontAssetTable");
        if (!get_fallbackFontAssetTable || !set_fallbackFontAssetTable) return;

        auto fallbackTable = get_fallbackFontAssetTable->Invoke<void*>(fontAsset);
        if (!fallbackTable) {
            static auto List_klass = Il2cppUtils::GetClass("mscorlib.dll", "System.Collections.Generic", "List`1");
            static auto List_ctor = Il2cppUtils::GetMethod("mscorlib.dll", "System.Collections.Generic", "List`1", ".ctor");
            if (!List_klass || !List_ctor) return;
            fallbackTable = List_klass->New<void*>();
            List_ctor->Invoke<void>(fallbackTable);
            set_fallbackFontAssetTable->Invoke<void>(fontAsset, fallbackTable);
        }

        if (InsertKoreanFontFirst(fallbackTable, source)) {
            if (Config::useRuntimeKoreanFont) {
                StripKoreanCharactersFromFontAsset(fontAsset);
            }
            updatedFontPtrs.emplace(fontAsset);
        }
    }

    void InjectKoreanFallbackIntoObservedFonts(const char* source) {
        for (auto fontAsset : observedFontAssets) {
            if (!fontAsset || !IsNativeObjectAlive(fontAsset)) continue;
            InjectKoreanFallbackIntoFontAsset(fontAsset, source);
        }
    }

    void AddOriginalFontFallbackToRuntimeFont(void* originalFontAsset, const char* source) {
        auto koFont = GetKoreanFontAsset();
        if (!Config::replaceFont || !Config::useRuntimeKoreanFont ||
            !koFont || !originalFontAsset || originalFontAsset == koFont) {
            return;
        }
        if (runtimeFallbackOriginalFontPtrs.contains(originalFontAsset)) return;
        if (runtimeFallbackOriginalFontPtrs.size() > 512) runtimeFallbackOriginalFontPtrs.clear();

        static auto get_fallbackFontAssetTable = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                        "TMP_FontAsset", "get_fallbackFontAssetTable");
        static auto set_fallbackFontAssetTable = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                        "TMP_FontAsset", "set_fallbackFontAssetTable");
        if (!get_fallbackFontAssetTable || !set_fallbackFontAssetTable) return;

        auto fallbackTable = get_fallbackFontAssetTable->Invoke<void*>(koFont);
        if (!fallbackTable) {
            static auto List_klass = Il2cppUtils::GetClass("mscorlib.dll", "System.Collections.Generic", "List`1");
            static auto List_ctor = Il2cppUtils::GetMethod("mscorlib.dll", "System.Collections.Generic", "List`1", ".ctor");
            if (!List_klass || !List_ctor) return;
            fallbackTable = List_klass->New<void*>();
            List_ctor->Invoke<void>(fallbackTable);
            set_fallbackFontAssetTable->Invoke<void>(koFont, fallbackTable);
        }

        if (InsertFontAssetFallback(fallbackTable, originalFontAsset, source, false)) {
            runtimeFallbackOriginalFontPtrs.emplace(originalFontAsset);
        }
    }

    void UpdateFont(void* TMP_Textself) {
        if (!Config::replaceFont) return;

        // 전역 폴백 등록 (koPatchedFontAsset 이 준비됐을 때만 실행)
        RegisterKoreanFontFallback();

        static auto get_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "get_font");
        static auto set_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "set_font");
        static auto get_name = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Object", "get_name");
        static auto SetAllDirty = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                         "TMPro", "TMP_Text", "SetAllDirty");
        static auto SetVerticesDirty = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                              "TMPro", "TMP_Text", "SetVerticesDirty");
        static auto SetMaterialDirty = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                              "TMPro", "TMP_Text", "SetMaterialDirty");
        static auto get_fontMaterial = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                              "TMPro", "TMP_Text", "get_fontMaterial");
        static auto get_fontSharedMaterial = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                                    "TMPro", "TMP_Text", "get_fontSharedMaterial");

        auto fontAsset = get_font->Invoke<void*>(TMP_Textself);
        if (!fontAsset) return;
        TrackObservedFontAsset(fontAsset);
        if (updatedFontPtrs.size() > 200) updatedFontPtrs.clear();
        if (forcedTextPtrs.size() > 1000) forcedTextPtrs.clear();

        auto koFont = GetKoreanFontAsset();
        if (!koFont) return;

        if (fontAsset != koFont && !updatedFontPtrs.contains(fontAsset)) {
            InjectKoreanFallbackIntoFontAsset(fontAsset, "TMP_Text.font.fallbackFontAssetTable");
        }
        if (fontAsset == koFont) {
            if (Config::useRuntimeKoreanFont) {
                if (SetAllDirty) {
                    SetAllDirty->Invoke<void>(TMP_Textself);
                } else {
                    if (SetVerticesDirty) SetVerticesDirty->Invoke<void>(TMP_Textself);
                    if (SetMaterialDirty) SetMaterialDirty->Invoke<void>(TMP_Textself);
                }
            }
            return;
        }

        if (Config::useRuntimeKoreanFont && set_font) {
            AddOriginalFontFallbackToRuntimeFont(fontAsset, "RuntimeKoreanFont.fallbackFontAssetTable");

            void* originalMaterial = nullptr;
            if (get_fontMaterial) {
                originalMaterial = get_fontMaterial->Invoke<void*>(TMP_Textself);
            }
            if (!originalMaterial && get_fontSharedMaterial) {
                originalMaterial = get_fontSharedMaterial->Invoke<void*>(TMP_Textself);
            }

            set_font->Invoke<void>(TMP_Textself, koFont);

            void* runtimeMaterial = nullptr;
            if (get_fontMaterial) {
                runtimeMaterial = get_fontMaterial->Invoke<void*>(TMP_Textself);
            }
            if (!runtimeMaterial && get_fontSharedMaterial) {
                runtimeMaterial = get_fontSharedMaterial->Invoke<void*>(TMP_Textself);
            }
            CopyRuntimeFontMaterialStyle(originalMaterial, runtimeMaterial);

            if (SetAllDirty) {
                SetAllDirty->Invoke<void>(TMP_Textself);
            } else {
                if (SetVerticesDirty) SetVerticesDirty->Invoke<void>(TMP_Textself);
                if (SetMaterialDirty) SetMaterialDirty->Invoke<void>(TMP_Textself);
            }

            if (!forcedTextPtrs.contains(TMP_Textself)) {
                forcedTextPtrs.emplace(TMP_Textself);
            }
        }
    }

    std::string MakeLocalizedText(const std::string& origText, const std::string& transText, bool hasTrans) {
        auto finalStr = FixLigature(hasTrans ? transText : origText);
        Local::DumpRemainingJapaneseText(finalStr);
        return finalStr;
    }

    DEFINE_HOOK(void, TMP_Text_PopulateTextBackingArray, (void* self, UnityResolve::UnityType::String* text, int start, int length)) {
        if (!text) {
            return TMP_Text_PopulateTextBackingArray_Orig(self, text, start, length);
        }

        static auto Substring = Il2cppUtils::GetMethod("mscorlib.dll", "System", "String", "Substring",
                                                       {"System.Int32", "System.Int32"});

        const std::string origText = Substring->Invoke<Il2cppString*>(text, start, length)->ToString();
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText) {
            const auto newText = UnityResolve::UnityType::String::New(finalStr);
            UpdateFont(self);
            TMP_Text_PopulateTextBackingArray_Orig(self, newText, 0, newText->length);
            UpdateFont(self);
            return;
        }

        if (Config::textTest) {
            TMP_Text_PopulateTextBackingArray_Orig(self, UnityResolve::UnityType::String::New("[TP]" + text->ToString()), start, length + 4);
        }
        else {
            TMP_Text_PopulateTextBackingArray_Orig(self, text, start, length);
        }
        UpdateFont(self);
    }

    DEFINE_HOOK(void, TMP_Text_set_text, (void* self, Il2cppString* value, void* mtd)) {
        if (!value) {
            return TMP_Text_set_text_Orig(self, value, mtd);
        }
        const std::string origText = value->ToString();
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText) {
            const auto newText = UnityResolve::UnityType::String::New(finalStr);
            UpdateFont(self);
            TMP_Text_set_text_Orig(self, newText, mtd);
            UpdateFont(self);
            return;
        }
        if (Config::textTest) {
            TMP_Text_set_text_Orig(self, UnityResolve::UnityType::String::New("[TT]" + origText), mtd);
        }
        else {
            TMP_Text_set_text_Orig(self, value, mtd);
        }
        UpdateFont(self);
    }

    DEFINE_HOOK(void, TMP_Text_SetText_1, (void* self, Il2cppString* sourceText, void* mtd)) {
        if (!sourceText) {
            return TMP_Text_SetText_1_Orig(self, sourceText, mtd);
        }
        const std::string origText = sourceText->ToString();
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText) {
            const auto newText = UnityResolve::UnityType::String::New(finalStr);
            UpdateFont(self);
            TMP_Text_SetText_1_Orig(self, newText, mtd);
            UpdateFont(self);
            return;
        }
        if (Config::textTest) {
            TMP_Text_SetText_1_Orig(self, UnityResolve::UnityType::String::New("[T1]" + origText), mtd);
        }
        else {
            TMP_Text_SetText_1_Orig(self, sourceText, mtd);
        }
        UpdateFont(self);
    }

    DEFINE_HOOK(void, TMP_Text_SetText_2, (void* self, Il2cppString* sourceText, bool syncTextInputBox, void* mtd)) {
		if (!sourceText) {
			return TMP_Text_SetText_2_Orig(self, sourceText, syncTextInputBox, mtd);
		}
		const std::string origText = sourceText->ToString();
		std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

		if (hasTrans || finalStr != origText) {
			const auto newText = UnityResolve::UnityType::String::New(finalStr);
			UpdateFont(self);
			TMP_Text_SetText_2_Orig(self, newText, syncTextInputBox, mtd);
            UpdateFont(self);
            return;
		}
		if (Config::textTest) {
			TMP_Text_SetText_2_Orig(self, UnityResolve::UnityType::String::New("[TS]" + sourceText->ToString()), syncTextInputBox, mtd);
		}
		else {
			TMP_Text_SetText_2_Orig(self, sourceText, syncTextInputBox, mtd);
		}
		UpdateFont(self);
    }

    DEFINE_HOOK(void, TextMeshProUGUI_Awake, (void* self, void* method)) {
        // Log::InfoFmt("TextMeshProUGUI_Awake at %p, self at %p", TextMeshProUGUI_Awake_Orig, self);

        const auto TMP_Text_klass = Il2cppUtils::GetClass("Unity.TextMeshPro.dll",
                                                                     "TMPro", "TMP_Text");
        const auto get_Text_method = TMP_Text_klass->Get<UnityResolve::Method>("get_text");
        const auto set_Text_method = TMP_Text_klass->Get<UnityResolve::Method>("set_text");
        const auto currText = get_Text_method->Invoke<UnityResolve::UnityType::String*>(self);
        if (currText) {
            //Log::InfoFmt("TextMeshProUGUI_Awake: %s", currText->ToString().c_str());
            std::string transText;
            bool hasTrans = Local::GetGenericText(currText->ToString(), &transText);
            std::string finalStr = MakeLocalizedText(currText->ToString(), transText, hasTrans);

            if (hasTrans || finalStr != currText->ToString()) {
                if (Config::textTest) {
                    set_Text_method->Invoke<void>(self, UnityResolve::UnityType::String::New("[TA]" + finalStr));
                }
                else {
                    set_Text_method->Invoke<void>(self, UnityResolve::UnityType::String::New(finalStr));
                }
            }
        }

        // set_font->Invoke<void>(self, font);
        UpdateFont(self);
        TextMeshProUGUI_Awake_Orig(self, method);
    }

    // TMP_FontAsset.Awake: UABEA로 교체한 'SourceSansPro-Regular' 에셋 캡처
    DEFINE_HOOK(void, TMP_FontAsset_Awake, (void* self)) {
        TrackObservedFontAsset(self);
        if (Config::replaceFont && Config::useRuntimeKoreanFont) {
            InjectKoreanFallbackIntoFontAsset(self, "TMP_FontAsset.Awake.before");
        }
        TMP_FontAsset_Awake_Orig(self);

        if (!Config::replaceFont) return;

        static auto get_name = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Object", "get_name");
        static auto get_sourceFontFile = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                "TMP_FontAsset", "get_sourceFontFile");
        auto nameStr = get_name->Invoke<Il2cppString*>(self);
        if (!nameStr) return;

        const std::string name = nameStr->ToString();
        Log::InfoFmt("[Font] TMP_FontAsset loaded: %s", name.c_str());

        if (name == "SourceSansPro-Regular SDF") {
            sourceSansProAsset = self;
            ActivateKoreanFont();
        } else if (name == "Solis-MK5 SDF" && !solisFontAsset) {
            solisFontAsset = self;
            Log::InfoFmt("[Font] Captured Solis-MK5 SDF as FaceInfo base");
            ActivateKoreanFont();
        }

        if (Config::useRuntimeKoreanFont) {
            InjectKoreanFallbackIntoFontAsset(self, "TMP_FontAsset.Awake.after");
        }
    }

    // Legacy UnityEngine.UI.Text hook（礼物/邮件等非TMP界面）
    DEFINE_HOOK(void, UIText_set_text, (void* self, Il2cppString* value)) {
        if (!value) {
            return UIText_set_text_Orig(self, value);
        }
        const std::string origText = value->ToString();
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText) {
            const auto newText = UnityResolve::UnityType::String::New(finalStr);
            return UIText_set_text_Orig(self, newText);
        }
        if (Config::textTest) {
            UIText_set_text_Orig(self, UnityResolve::UnityType::String::New("[UI]" + origText));
        }
        else {
            UIText_set_text_Orig(self, value);
        }
    }

    // TMP_Text.SetCharArray(char[], int, int) — 礼物/邮件描述文字通过此路径设置
    DEFINE_HOOK(void, TMP_Text_SetCharArray, (void* self, void* charArray, int start, int count, void* mtd)) {
        if (charArray && start >= 0 && count > 0) {
            // IL2CPP char[] elements are uint16_t (UTF-16)
            auto arr = reinterpret_cast<UnityResolve::UnityType::Array<uint16_t>*>(charArray);
            // 边界检查：确保 start+count 不超出数组长度
            if (static_cast<uintptr_t>(start + count) <= arr->max_length) {
                auto rawData = arr->GetData();
                if (rawData) {
                    // rawData 是 uintptr_t（字节地址），每个 char16_t 占 2 字节
                    // 必须用 start * sizeof(char16_t) 而非直接 + start（否则偏移量减半）
                    const std::u16string u16(
                        reinterpret_cast<const char16_t*>(rawData + static_cast<uintptr_t>(start) * sizeof(char16_t)),
                        static_cast<size_t>(count));
                    const std::string origText = Misc::ToUTF8(u16);
                    std::string transText;
                    bool hasTrans = Local::GetGenericText(origText, &transText);
                    std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

                    if (hasTrans || finalStr != origText) {
                        UpdateFont(self);
                        TMP_Text_set_text_Orig(self, Il2cppString::New(finalStr), nullptr);
                        UpdateFont(self);
                        return;
                    }
                    if (Config::textTest) {
                        UpdateFont(self);
                        TMP_Text_set_text_Orig(self, Il2cppString::New("[CA]" + origText), nullptr);
                        UpdateFont(self);
                        return;
                    }
                }
            }
        }
        TMP_Text_SetCharArray_Orig(self, charArray, start, count, mtd);
    }

    DEFINE_HOOK(void, TextField_set_value, (void* self, Il2cppString* value)) {
        if (value) {
            std::string transText;
            if (Local::GetGenericText(value->ToString(), &transText)) {
                Local::DumpRemainingJapaneseText(transText);
                return TextField_set_value_Orig(self, UnityResolve::UnityType::String::New(transText));
            }
        }
        if (value) {
            Local::DumpRemainingJapaneseText(value->ToString());
        }
        TextField_set_value_Orig(self, value);
    }

    // 未使用的 Hook
    DEFINE_HOOK(void, EffectGroup_ctor, (void* self, void* mtd)) {
        // auto self_klass = Il2cppUtils::get_class_from_instance(self);
        // Log::DebugFmt("EffectGroup_ctor: self: %s::%s", self_klass->namespaze, self_klass->name);
        EffectGroup_ctor_Orig(self, mtd);
    }

    // 用于本地化 MasterDB
    DEFINE_HOOK(void, MessageExtensions_MergeFrom, (void* message, void* span, void* mtd)) {
        MessageExtensions_MergeFrom_Orig(message, span, mtd);
        if (message) {
            auto ret_klass = Il2cppUtils::get_class_from_instance(message);
            if (ret_klass) {
                const std::string tableName = ret_klass->name ? ret_klass->name : "<unknown>";
                if (Config::debugMasterDbLog) {
                    static std::unordered_set<std::string> loggedMasterTables;
                    if (loggedMasterTables.insert(tableName).second) {
                        Log::DebugFmt("ResourceDebug[MasterDB]: table=%s", tableName.c_str());
                    }
                }
                MasterLocal::LocalizeMasterItem(message, tableName);
            }
        }
    }

    DEFINE_HOOK(Il2cppString*, OctoCaching_GetResourceFileName, (void* data, void* method)) {
        auto ret = OctoCaching_GetResourceFileName_Orig(data, method);
        //Log::DebugFmt("OctoCaching_GetResourceFileName: %s", ret->ToString().c_str());
        return ret;
    }

    std::unordered_map<std::string, void*> sprite_cache;
    std::unordered_map<std::string, void*> texture_cache;

    std::unordered_set<std::string> sprite_negative_cache;
    thread_local bool applying_image_replacement = false;

    bool IsUnityObjectAlive(void* obj) {
        if (!obj) return false;
        return IsNativeObjectAlive(obj);
    }

    void MarkReplacementAssetPersistent(void* obj) {
        if (!obj || !IsUnityObjectAlive(obj)) return;
        static auto set_hide_flags = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "set_hideFlags", {"UnityEngine.HideFlags"});
        if (set_hide_flags) {
            constexpr int DontUnloadUnusedAsset = 32;
            set_hide_flags->Invoke<void>(obj, DontUnloadUnusedAsset);
        }
    }

    void* GetAliveCachedAsset(std::unordered_map<std::string, void*>& cache, const std::string& name) {
        auto it = cache.find(name);
        if (it == cache.end()) return nullptr;
        if (IsUnityObjectAlive(it->second)) return it->second;
        cache.erase(it);
        return nullptr;
    }

    std::string GetObjectName(void* obj) {
        if (!obj) return "";
        if (!IsUnityObjectAlive(obj)) return "";
        static auto get_name = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "get_name");
        if (!get_name) return "";
        auto nameStr = get_name->Invoke<Il2cppString*>(obj);
        if (!nameStr) return "";
        std::string name = nameStr->ToString();
        const std::string cloneSuffix = "(Clone)";
        if (name.length() >= cloneSuffix.length() && 
            name.compare(name.length() - cloneSuffix.length(), cloneSuffix.length(), cloneSuffix) == 0) {
            name = name.substr(0, name.length() - cloneSuffix.length());
        }
        return name;
    }

    void* CreateSpriteFromBytes(const std::vector<uint8_t>& bytes) {
        static auto byte_klass = Il2cppUtils::GetClass("mscorlib.dll", "System", "Byte");
        auto il2cpp_bytes = UnityResolve::UnityType::Array<uint8_t>::New(byte_klass, bytes.size());
        std::memcpy(reinterpret_cast<void*>(il2cpp_bytes->GetData()), bytes.data(), bytes.size());

        static auto texture2d_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
        static auto texture2d_ctor = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D", ".ctor", {"System.Int32", "System.Int32"});

        auto tex = texture2d_klass->New<void*>();
        texture2d_ctor->Invoke<void>(tex, 2, 2);
        MarkReplacementAssetPersistent(tex);

        static auto image_conversion_class = Il2cppUtils::GetClass("UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion");
        static auto load_image_ptr = reinterpret_cast<bool (*)(void*, void*, bool, void*)>(Il2cppUtils::GetMethodPointer("UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion", "LoadImage", {"UnityEngine.Texture2D", "System.Byte[]", "System.Boolean"}));
        bool load_success = false;
        if (load_image_ptr) {
            load_success = load_image_ptr(tex, il2cpp_bytes, false, nullptr);
        } else {
            static auto load_image = image_conversion_class ? image_conversion_class->Get<UnityResolve::Method>("LoadImage", {"UnityEngine.Texture2D", "System.Byte[]"}) : nullptr;
            if (load_image) {
                load_success = load_image->Invoke<bool>(nullptr, tex, il2cpp_bytes);
            }
        }
        // Log::InfoFmt("LoadImage success: %d", load_success);

        static auto sprite_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Sprite");
        static auto sprite_create = sprite_klass->Get<UnityResolve::Method>("Create", {"UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2"});

        if (sprite_create) {
            int w = 0, h = 0;
            if (bytes.size() > 24 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
                w = (bytes[16] << 24) | (bytes[17] << 16) | (bytes[18] << 8) | bytes[19];
                h = (bytes[20] << 24) | (bytes[21] << 16) | (bytes[22] << 8) | bytes[23];
            } else {
                static auto get_width = texture2d_klass->Get<UnityResolve::Method>("get_width");
                static auto get_height = texture2d_klass->Get<UnityResolve::Method>("get_height");
                if (get_width) w = get_width->Invoke<int>(tex);
                if (get_height) h = get_height->Invoke<int>(tex);
            }
            // Log::InfoFmt("CreateSpriteFromBytes parsed w=%d, h=%d", w, h);

            struct Rect { float x, y, width, height; } rect { 0, 0, (float)w, (float)h };
            struct Vector2 { float x, y; } pivot { 0.5f, 0.5f };

            if (sprite_create) {
                void* args[3] = { tex, &rect, &pivot };
                auto sprite = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", sprite_create->address, nullptr, args, nullptr);
                MarkReplacementAssetPersistent(sprite);
                return sprite;
            } else {
                // Log::InfoFmt("Failed to find Sprite.Create MethodInfo");
            }
        } else {
            // Log::InfoFmt("Failed to resolve Sprite.Create method!");
        }
        return nullptr;
    }

    void* CreateTextureFromBytes(const std::vector<uint8_t>& bytes) {
        static auto byte_klass = Il2cppUtils::GetClass("mscorlib.dll", "System", "Byte");
        auto il2cpp_bytes = UnityResolve::UnityType::Array<uint8_t>::New(byte_klass, bytes.size());
        std::memcpy(reinterpret_cast<void*>(il2cpp_bytes->GetData()), bytes.data(), bytes.size());

        static auto texture2d_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
        static auto texture2d_ctor = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D", ".ctor", {"System.Int32", "System.Int32"});
        static auto texture2d_ctor_full = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D", ".ctor",
                                                                 {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean"});

        auto tex = texture2d_klass->New<void*>();
        if (texture2d_ctor_full) {
            texture2d_ctor_full->Invoke<void>(tex, 2, 2, 4, false);
        } else {
            texture2d_ctor->Invoke<void>(tex, 2, 2);
        }
        MarkReplacementAssetPersistent(tex);

        static auto image_conversion_class = Il2cppUtils::GetClass("UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion");
        static auto load_image_ptr = reinterpret_cast<bool (*)(void*, void*, bool, void*)>(Il2cppUtils::GetMethodPointer("UnityEngine.ImageConversionModule.dll", "UnityEngine", "ImageConversion", "LoadImage", {"UnityEngine.Texture2D", "System.Byte[]", "System.Boolean"}));
        static auto load_image_3 = image_conversion_class ? image_conversion_class->Get<UnityResolve::Method>("LoadImage", {"UnityEngine.Texture2D", "System.Byte[]", "System.Boolean"}) : nullptr;
        static auto load_image_2 = image_conversion_class ? image_conversion_class->Get<UnityResolve::Method>("LoadImage", {"UnityEngine.Texture2D", "System.Byte[]"}) : nullptr;

        bool load_success = false;
        if (load_image_ptr) {
            load_success = load_image_ptr(tex, il2cpp_bytes, false, nullptr);
        } else if (load_image_3) {
            load_success = load_image_3->Invoke<bool>(nullptr, tex, il2cpp_bytes, false);
        }
        if (!load_success && load_image_2) {
            load_success = load_image_2->Invoke<bool>(nullptr, tex, il2cpp_bytes);
        }
        return load_success ? tex : nullptr;
    }

    void SetImagePreserveAspect(void* self) {
        static auto set_preserve_aspect = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "Image", "set_preserveAspect", {"System.Boolean"});
        if (set_preserve_aspect) set_preserve_aspect->Invoke<void>(self, true);
    }

    bool LoadReplacementImageBytes(const std::string& name, std::vector<uint8_t>* bytes) {
        if (name.empty()) return false;
        return Local::GetResourceBytes(name + ".png", bytes) || Local::GetResourceBytes(name, bytes);
    }

    void* GetOrCreateReplacementSprite(const std::string& name, bool rememberMiss) {
        if (name.empty()) return nullptr;
        if (rememberMiss && sprite_negative_cache.contains(name)) return nullptr;
        if (auto cachedSprite = GetAliveCachedAsset(sprite_cache, name)) return cachedSprite;

        std::vector<uint8_t> bytes;
        if (!LoadReplacementImageBytes(name, &bytes)) {
            if (rememberMiss) sprite_negative_cache.insert(name);
            return nullptr;
        }

        auto newSprite = CreateSpriteFromBytes(bytes);
        if (newSprite) sprite_cache[name] = newSprite;
        return newSprite;
    }

    void* GetOrCreateReplacementTexture(const std::string& name, bool rememberMiss) {
        if (name.empty()) return nullptr;
        if (rememberMiss && sprite_negative_cache.contains(name)) return nullptr;
        if (auto cachedTexture = GetAliveCachedAsset(texture_cache, name)) return cachedTexture;

        std::vector<uint8_t> bytes;
        if (!LoadReplacementImageBytes(name, &bytes)) {
            if (rememberMiss) sprite_negative_cache.insert(name);
            return nullptr;
        }

        auto newTexture = CreateTextureFromBytes(bytes);
        if (newTexture) texture_cache[name] = newTexture;
        return newTexture;
    }

    DEFINE_HOOK(void, Graphic_OnEnable, (void* self)) {
        Graphic_OnEnable_Orig(self);
        if (!Config::replaceImages) return;

        auto klass = Il2cppUtils::get_class_from_instance(self);
        if (!klass) return;
        std::string className = klass->name;

        if (className == "Image") {
            static auto get_sprite = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "Image", "get_sprite");
            if (!get_sprite) return;
            auto sprite = get_sprite->Invoke<void*>(self);
            if (!sprite) return;
            std::string name = GetObjectName(sprite);
            if (auto replacementSprite = GetOrCreateReplacementSprite(name, true)) {
                LogImageResourceDebug("Graphic.OnEnable.Image", self, name, true);
                static auto set_sprite = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "Image", "set_sprite");
                applying_image_replacement = true;
                set_sprite->Invoke<void>(self, replacementSprite);
                applying_image_replacement = false;
                SetImagePreserveAspect(self);
            }
        } else if (className == "RawImage") {
            static auto get_texture = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "RawImage", "get_texture");
            if (!get_texture) return;
            auto texture = get_texture->Invoke<void*>(self);
            if (!texture) return;
            std::string name = GetObjectName(texture);
            if (auto replacementTexture = GetOrCreateReplacementTexture(name, true)) {
                LogImageResourceDebug("Graphic.OnEnable.RawImage", self, name, true);
                static auto set_texture = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "RawImage", "set_texture");
                applying_image_replacement = true;
                set_texture->Invoke<void>(self, replacementTexture);
                applying_image_replacement = false;
            }
        }
    }

    DEFINE_HOOK(void, Image_set_sprite, (void* self, void* value, void* method)) {
        if (applying_image_replacement) return Image_set_sprite_Orig(self, value, method);
        if (!Config::replaceImages) return Image_set_sprite_Orig(self, value, method);
        if (value) {
            std::string name = GetObjectName(value);
            if (!name.empty()) {
                LogImageResourceDebug("Image.set_sprite", self, name, false);
                if (auto replacementSprite = GetOrCreateReplacementSprite(name, false)) {
                    LogImageResourceDebug("Image.set_sprite", self, name, true);
                    Image_set_sprite_Orig(self, replacementSprite, method);
                    SetImagePreserveAspect(self);
                    return;
                }
            }
        }
        return Image_set_sprite_Orig(self, value, method);
    }

    DEFINE_HOOK(void, Image_set_overrideSprite, (void* self, void* value, void* method)) {
        if (applying_image_replacement) return Image_set_overrideSprite_Orig(self, value, method);
        if (!Config::replaceImages) return Image_set_overrideSprite_Orig(self, value, method);
        if (value) {
            std::string name = GetObjectName(value);
            if (!name.empty()) {
                LogImageResourceDebug("Image.set_overrideSprite", self, name, false);
                if (auto replacementSprite = GetOrCreateReplacementSprite(name, false)) {
                    LogImageResourceDebug("Image.set_overrideSprite", self, name, true);
                    Image_set_overrideSprite_Orig(self, replacementSprite, method);
                    SetImagePreserveAspect(self);
                    return;
                }
            }
        }
        return Image_set_overrideSprite_Orig(self, value, method);
    }

    DEFINE_HOOK(void, RawImage_set_texture, (void* self, void* value, void* method)) {
        if (applying_image_replacement) return RawImage_set_texture_Orig(self, value, method);
        if (!Config::replaceImages) return RawImage_set_texture_Orig(self, value, method);
        if (value) {
            std::string name = GetObjectName(value);
            if (!name.empty()) {
                LogImageResourceDebug("RawImage.set_texture", self, name, false);
                if (auto replacementTexture = GetOrCreateReplacementTexture(name, false)) {
                    LogImageResourceDebug("RawImage.set_texture", self, name, true);
                    return RawImage_set_texture_Orig(self, replacementTexture, method);
                }
            }
        }
        return RawImage_set_texture_Orig(self, value, method);
    }


    DEFINE_HOOK(void*, AssetBundle_LoadAsset, (void* self, Il2cppString* name, void* method)) {
        auto ret = AssetBundle_LoadAsset_Orig(self, name, method);
        if (Config::debugImageResourceLog && name) {
            Log::DebugFmt("ResourceDebug[AssetBundle.LoadAsset]: asset=%s", name->ToString().c_str());
        }
        return ret;
    }


    DEFINE_HOOK(void, OctoResourceLoader_LoadFromCacheOrDownload,
                (void* self, Il2cppString* resourceName, void* onComplete, void* onProgress, void* method)) {

        if (!resourceName) {
            return OctoResourceLoader_LoadFromCacheOrDownload_Orig(self, resourceName, onComplete, onProgress, method);
        }

        const auto resourceNameStr = resourceName->ToString();
        HoshimiLocal::Log::DebugFmt("OctoResourceLoader_LoadFromCacheOrDownload: %s\n", resourceNameStr.c_str());

        std::string replaceStr;
        if (Local::GetResourceText(resourceNameStr, &replaceStr)) {
            const auto onComplete_klass = Il2cppUtils::get_class_from_instance(onComplete);
            const auto onComplete_invoke_mtd = UnityResolve::Invoke<Il2cppUtils::MethodInfo*>(
                    "il2cpp_class_get_method_from_name", onComplete_klass, "Invoke", 2);
            if (onComplete_invoke_mtd) {
                const auto onComplete_invoke = reinterpret_cast<void (*)(void*, Il2cppString*, void*)>(
                        onComplete_invoke_mtd->methodPointer
                );
                onComplete_invoke(onComplete, UnityResolve::UnityType::String::New(replaceStr), nullptr);
                return;
            }
        }

        return OctoResourceLoader_LoadFromCacheOrDownload_Orig(self, resourceName, onComplete, onProgress, method);
    }

    DEFINE_HOOK(void, OnDownloadProgress_Invoke, (void* self, Il2cppString* name, uint64_t receivedLength, uint64_t contentLength)) {
        if (name) {
            Log::DebugFmt("OnDownloadProgress_Invoke: %s, %lu/%lu", name->ToString().c_str(), receivedLength, contentLength);
        }
        OnDownloadProgress_Invoke_Orig(self, name, receivedLength, contentLength);
    }

    // UnHooked
    DEFINE_HOOK(UnityResolve::UnityType::String*, UI_I18n_GetOrDefault, (void* self,
            UnityResolve::UnityType::String* key, UnityResolve::UnityType::String* defaultKey, void* method)) {

        auto ret = UI_I18n_GetOrDefault_Orig(self, key, defaultKey, method);

        // Log::DebugFmt("UI_I18n_GetOrDefault: key: %s, default: %s, result: %s", key->ToString().c_str(), defaultKey->ToString().c_str(), ret->ToString().c_str());

        return ret;
        // return UnityResolve::UnityType::String::New("[I18]" + ret->ToString());
    }

    enum class SolisMasterIdType {
        CostumeId,
        HairId,
        PhotoPoseId,
        MusicId
    };

    std::vector<std::string> GetSolisMasterIdAll(SolisMasterIdType getType = SolisMasterIdType::CostumeId,
                                                 const std::string& characterId = "") {
        std::vector<std::string> ret{};

        const char* masterGetterName = getType == SolisMasterIdType::CostumeId ? "get_CostumeMaster" :
                                       getType == SolisMasterIdType::HairId ? "get_HairMaster" :
                                       getType == SolisMasterIdType::PhotoPoseId ? "get_PhotoPoseMaster" : "get_MusicMaster";
        const char* masterClassName = getType == SolisMasterIdType::CostumeId ? "CostumeMaster" :
                                      getType == SolisMasterIdType::HairId ? "HairMaster" :
                                      getType == SolisMasterIdType::PhotoPoseId ? "PhotoPoseMaster" : "MusicMaster";
        const char* protoClassName = getType == SolisMasterIdType::CostumeId ? "Costume" :
                                     getType == SolisMasterIdType::HairId ? "Hair" :
                                     getType == SolisMasterIdType::PhotoPoseId ? "PhotoPose" : "Music";

        static auto get_CostumeMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                               "MasterManager", "get_CostumeMaster");
        static auto get_MusicMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                             "MasterManager", "get_MusicMaster");
        static auto get_HairMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                            "MasterManager", "get_HairMaster");
        static auto get_PhotoPoseMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                 "MasterManager", "get_PhotoPoseMaster");
        static auto CostumeMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                               "CostumeMaster", "GetAllWithSortByKey",
                                                                               {"Solis.Common.Master.CostumeSortType"});
        static auto MusicMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                             "MusicMaster", "GetAllWithSortByKey",
                                                                             {"Solis.Common.Master.MusicSortType"});
        static auto HairMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                            "HairMaster", "GetAllWithSortByKey",
                                                                            {"Solis.Common.Master.HairSortType"});
        static auto PhotoPoseMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                                 "PhotoPoseMaster", "GetAllWithSortByKey",
                                                                                 {"Solis.Common.Master.PhotoPoseSortType"});
        static auto Costume_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                            "Costume", "get_Id");
        static auto Costume_get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                     "Costume", "get_CharacterId");
        static auto Music_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                          "Music", "get_Id");
        static auto Music_get_Is3DAvailable = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                     "Music", "get_Is3DAvailable");
        static auto Hair_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                         "Hair", "get_Id");
        static auto Hair_get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                  "Hair", "get_CharacterId");
        static auto PhotoPose_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                              "PhotoPose", "get_Id");

        auto getMaster = getType == SolisMasterIdType::CostumeId ? get_CostumeMaster :
                         getType == SolisMasterIdType::HairId ? get_HairMaster :
                         getType == SolisMasterIdType::PhotoPoseId ? get_PhotoPoseMaster : get_MusicMaster;
        auto getAllWithSortByKey = getType == SolisMasterIdType::CostumeId ? CostumeMaster_GetAllWithSortByKey :
                                   getType == SolisMasterIdType::HairId ? HairMaster_GetAllWithSortByKey :
                                   getType == SolisMasterIdType::PhotoPoseId ? PhotoPoseMaster_GetAllWithSortByKey : MusicMaster_GetAllWithSortByKey;
        auto getId = getType == SolisMasterIdType::CostumeId ? Costume_get_Id :
                     getType == SolisMasterIdType::HairId ? Hair_get_Id :
                     getType == SolisMasterIdType::PhotoPoseId ? PhotoPose_get_Id : Music_get_Id;
        if (!getMaster || !getAllWithSortByKey || !getId) {
            Log::ErrorFmt("GetSolisMasterIdAll failed: %s/%s/%s missing", masterGetterName, masterClassName, protoClassName);
            return ret;
        }

        auto master = getMaster->Invoke<void*>(nullptr);
        if (!master) {
            Log::ErrorFmt("%s failed: %p", masterGetterName, master);
            return ret;
        }

        auto masterList = getAllWithSortByKey->Invoke<void*>(master, 0x0);
        if (!masterList) {
            Log::ErrorFmt("%s.GetAllWithSortByKey failed: %p", masterClassName, masterList);
            return ret;
        }

        Il2cppUtils::Tools::CSListEditor<void*> listEditor(masterList);
        for (auto item : listEditor) {
            if (!item) continue;

            if (getType == SolisMasterIdType::CostumeId) {
                if (!characterId.empty() && Costume_get_CharacterId) {
                    auto itemCharacterId = Costume_get_CharacterId->Invoke<Il2cppString*>(item);
                    if (!itemCharacterId || itemCharacterId->ToString() != characterId) continue;
                }
            }
            else if (getType == SolisMasterIdType::HairId) {
                if (!characterId.empty() && Hair_get_CharacterId) {
                    auto itemCharacterId = Hair_get_CharacterId->Invoke<Il2cppString*>(item);
                    if (!itemCharacterId || itemCharacterId->ToString() != characterId) continue;
                }
            }
            else if (getType == SolisMasterIdType::MusicId) {
                if (Music_get_Is3DAvailable && !Music_get_Is3DAvailable->Invoke<bool>(item)) continue;
            }

            auto id = getId->Invoke<Il2cppString*>(item);
            if (!id) continue;
            auto idStr = id->ToString();
            if (!idStr.empty() && std::find(ret.begin(), ret.end(), idStr) == ret.end()) {
                ret.emplace_back(idStr);
            }
        }

        return ret;
    }

    bool IsSolisMasterIdExists(const std::string& id, SolisMasterIdType getType) {
        if (id.empty()) return false;
        static std::unordered_set<std::string> costumeIds{};
        static std::unordered_set<std::string> hairIds{};
        static std::unordered_set<std::string> photoPoseIds{};
        static std::unordered_set<std::string> musicIds{};

        auto& ids = getType == SolisMasterIdType::CostumeId ? costumeIds :
                    getType == SolisMasterIdType::HairId ? hairIds :
                    getType == SolisMasterIdType::PhotoPoseId ? photoPoseIds : musicIds;
        if (ids.empty()) {
            auto allIds = GetSolisMasterIdAll(getType);
            ids.insert(allIds.begin(), allIds.end());
        }
        return ids.contains(id);
    }

    void* AddSolisIdsToUserDataCollectionFromMaster(void* origList, std::vector<std::string>& allIds,
                                                    const char* userClassName, UnityResolve::Method* get_Id,
                                                    UnityResolve::Method* set_Id, UnityResolve::Method* Clone) {
        if (!origList) return origList;

        std::unordered_set<std::string> existIds{};
        Il2cppUtils::Tools::CSListEditor listEditor(origList);
        auto origCount = listEditor.get_Count();

        for (auto i : listEditor) {
            auto id = get_Id->Invoke<Il2cppString*>(i);
            if (!id) continue;
            existIds.emplace(id->ToString());
        }

        static auto UserCostume_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                              "UserCostume");
        static auto UserHair_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                           "UserHair");
        static auto UserPhotoPose_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                "UserPhotoPose");
        static auto UserMusic_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                            "UserMusic");
        static auto UserCostume_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                              "UserCostume", ".ctor");
        static auto UserHair_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                           "UserHair", ".ctor");
        static auto UserPhotoPose_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                "UserPhotoPose", ".ctor");
        static auto UserMusic_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                            "UserMusic", ".ctor");

        auto className = std::string(userClassName);
        auto klass = className == "UserCostume" ? UserCostume_klass :
                     className == "UserHair" ? UserHair_klass :
                     className == "UserPhotoPose" ? UserPhotoPose_klass : UserMusic_klass;
        auto ctor = className == "UserCostume" ? UserCostume_ctor :
                    className == "UserHair" ? UserHair_ctor :
                    className == "UserPhotoPose" ? UserPhotoPose_ctor : UserMusic_ctor;

        for (auto& i : allIds) {
            if (i.empty()) continue;
            if (existIds.contains(i)) continue;

            void* userData = nullptr;
            if (origCount > 0 && Clone) {
                userData = Clone->Invoke<void*>(listEditor.get_Item(0));
            }
            else if (klass && ctor) {
                userData = klass->New<void*>();
                ctor->Invoke<void>(userData);
            }
            if (!userData) continue;

            set_Id->Invoke<void>(userData, Il2cppString::New(i));
            listEditor.Add(userData);
            existIds.emplace(i);
        }
        return origList;
    }

    void* AddSolisCostumesToUserCollection(void* origList, const std::string& characterId = "") {
        if (!origList) return origList;

        static auto UserCostume_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                               "UserCostume", "Clone");
        static auto UserCostume_get_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                       "UserCostume", "get_CostumeId");
        static auto UserCostume_set_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                       "UserCostume", "set_CostumeId");
        if (!UserCostume_Clone || !UserCostume_get_CostumeId || !UserCostume_set_CostumeId) {
            Log::Error("AddSolisCostumesToUserCollection failed: UserCostume methods missing");
            return origList;
        }

        auto allIds = GetSolisMasterIdAll(SolisMasterIdType::CostumeId, characterId);
        return AddSolisIdsToUserDataCollectionFromMaster(origList, allIds, "UserCostume",
                                                         UserCostume_get_CostumeId, UserCostume_set_CostumeId,
                                                         UserCostume_Clone);
    }

    void* AddSolisHairsToUserCollection(void* origList, const std::string& characterId = "") {
        if (!origList) return origList;

        static auto UserHair_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                            "UserHair", "Clone");
        static auto UserHair_get_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                 "UserHair", "get_HairId");
        static auto UserHair_set_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                 "UserHair", "set_HairId");
        if (!UserHair_Clone || !UserHair_get_HairId || !UserHair_set_HairId) {
            Log::Error("AddSolisHairsToUserCollection failed: UserHair methods missing");
            return origList;
        }

        auto allIds = GetSolisMasterIdAll(SolisMasterIdType::HairId, characterId);
        return AddSolisIdsToUserDataCollectionFromMaster(origList, allIds, "UserHair",
                                                         UserHair_get_HairId, UserHair_set_HairId,
                                                         UserHair_Clone);
    }

    void* AddSolisMusicsToUserCollection(void* origList) {
        if (!origList) return origList;

        static auto UserMusic_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                            "UserMusic", "Clone");
        static auto UserMusic_get_MusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                   "UserMusic", "get_MusicId");
        static auto UserMusic_set_MusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                   "UserMusic", "set_MusicId");
        if (!UserMusic_Clone || !UserMusic_get_MusicId || !UserMusic_set_MusicId) {
            Log::Error("AddSolisMusicsToUserCollection failed: UserMusic methods missing");
            return origList;
        }

        auto allIds = GetSolisMasterIdAll(SolisMasterIdType::MusicId);
        return AddSolisIdsToUserDataCollectionFromMaster(origList, allIds, "UserMusic",
                                                         UserMusic_get_MusicId, UserMusic_set_MusicId,
                                                         UserMusic_Clone);
    }

    void* AddSolisPhotoPosesToUserCollection(void* origList) {
        if (!origList) return origList;

        static auto UserPhotoPose_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                 "UserPhotoPose", "Clone");
        static auto UserPhotoPose_get_PhotoPoseId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                           "UserPhotoPose", "get_PhotoPoseId");
        static auto UserPhotoPose_set_PhotoPoseId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                           "UserPhotoPose", "set_PhotoPoseId");
        if (!UserPhotoPose_Clone || !UserPhotoPose_get_PhotoPoseId || !UserPhotoPose_set_PhotoPoseId) {
            Log::Error("AddSolisPhotoPosesToUserCollection failed: UserPhotoPose methods missing");
            return origList;
        }

        auto allIds = GetSolisMasterIdAll(SolisMasterIdType::PhotoPoseId);
        return AddSolisIdsToUserDataCollectionFromMaster(origList, allIds, "UserPhotoPose",
                                                         UserPhotoPose_get_PhotoPoseId, UserPhotoPose_set_PhotoPoseId,
                                                         UserPhotoPose_Clone);
    }

    void* NewSolisStringList(const std::vector<std::string>& ids) {
        static auto List_String_klass = Il2cppUtils::get_system_class_from_reflection_type_str(
            "System.Collections.Generic.List`1[System.String]");
        static auto List_String_ctor_mtd = List_String_klass ?
                Il2cppUtils::il2cpp_class_get_method_from_name(List_String_klass, ".ctor", 0) : nullptr;
        static auto List_String_ctor = List_String_ctor_mtd ?
                reinterpret_cast<void (*)(void*, void*)>(List_String_ctor_mtd->methodPointer) : nullptr;
        if (!List_String_klass || !List_String_ctor) return nullptr;

        auto list = UnityResolve::Invoke<void*>("il2cpp_object_new", List_String_klass);
        List_String_ctor(list, List_String_ctor_mtd);
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(list);
        for (const auto& id : ids) {
            if (!id.empty()) editor.Add(Il2cppString::New(id));
        }
        return list;
    }

    bool GetSolisDefaultLiveIdVectors(void* characterIds, std::vector<std::string>& costumeIds,
                                      std::vector<std::string>& hairIds) {
        if (!characterIds) return false;
        static auto get_CharacterMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                 "MasterManager", "get_CharacterMaster");
        static auto get_CostumeMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                               "MasterManager", "get_CostumeMaster");
        static auto CharacterMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                 "Solis.Common.Master",
                                                                                 "CharacterMaster", "GetAllWithSortByKey",
                                                                                 {"Solis.Common.Master.CharacterSortType"});
        static auto CostumeMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                               "Solis.Common.Master",
                                                                               "CostumeMaster", "GetAllWithSortByKey",
                                                                               {"Solis.Common.Master.CostumeSortType"});
        static auto Character_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                              "Character", "get_Id");
        static auto Costume_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                            "Costume", "get_Id");
        static auto Character_get_DefaultLiveCostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                "Solis.Common.Proto.Master",
                                                                                "Character", "get_DefaultLiveCostumeId");
        static auto Costume_get_DefaultHairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                       "Solis.Common.Proto.Master",
                                                                       "Costume", "get_DefaultHairId");
        if (!get_CharacterMaster || !get_CostumeMaster || !CharacterMaster_GetAllWithSortByKey ||
            !CostumeMaster_GetAllWithSortByKey || !Character_get_Id || !Costume_get_Id ||
            !Character_get_DefaultLiveCostumeId || !Costume_get_DefaultHairId) {
            Log::Error("GetSolisDefaultLiveIdVectors failed: master methods missing");
            return false;
        }

        auto characterMaster = get_CharacterMaster->Invoke<void*>(nullptr);
        auto costumeMaster = get_CostumeMaster->Invoke<void*>(nullptr);
        if (!characterMaster || !costumeMaster) return false;

        static std::unordered_map<std::string, std::string> defaultLiveCostumeByCharacter{};
        static std::unordered_map<std::string, std::string> defaultHairByCostume{};
        if (defaultLiveCostumeByCharacter.empty()) {
            auto characterList = CharacterMaster_GetAllWithSortByKey->Invoke<void*>(characterMaster, 0x0);
            if (!characterList) return false;

            Il2cppUtils::Tools::CSListEditor<void*> characterListEditor(characterList);
            for (auto character : characterListEditor) {
                if (!character) continue;
                auto characterId = Character_get_Id->Invoke<Il2cppString*>(character);
                auto costumeId = Character_get_DefaultLiveCostumeId->Invoke<Il2cppString*>(character);
                if (!characterId || !costumeId) continue;

                auto characterIdStr = characterId->ToString();
                auto costumeIdStr = costumeId->ToString();
                if (!characterIdStr.empty() && !costumeIdStr.empty()) {
                    defaultLiveCostumeByCharacter[characterIdStr] = costumeIdStr;
                }
            }
        }
        if (defaultHairByCostume.empty()) {
            auto costumeList = CostumeMaster_GetAllWithSortByKey->Invoke<void*>(costumeMaster, 0x0);
            if (!costumeList) return false;

            Il2cppUtils::Tools::CSListEditor<void*> costumeListEditor(costumeList);
            for (auto costume : costumeListEditor) {
                if (!costume) continue;
                auto costumeId = Costume_get_Id->Invoke<Il2cppString*>(costume);
                auto hairId = Costume_get_DefaultHairId->Invoke<Il2cppString*>(costume);
                if (!costumeId || !hairId) continue;

                auto costumeIdStr = costumeId->ToString();
                auto hairIdStr = hairId->ToString();
                if (!costumeIdStr.empty() && !hairIdStr.empty()) {
                    defaultHairByCostume[costumeIdStr] = hairIdStr;
                }
            }
        }
        if (defaultLiveCostumeByCharacter.empty() || defaultHairByCostume.empty()) return false;

        costumeIds.clear();
        hairIds.clear();
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> characterIdEditor(characterIds);
        for (auto characterId : characterIdEditor) {
            if (!characterId) return false;

            auto characterIdStr = characterId->ToString();
            auto costumeIt = defaultLiveCostumeByCharacter.find(characterIdStr);
            if (costumeIt == defaultLiveCostumeByCharacter.end()) return false;

            auto hairIt = defaultHairByCostume.find(costumeIt->second);
            if (hairIt == defaultHairByCostume.end()) return false;

            costumeIds.emplace_back(costumeIt->second);
            hairIds.emplace_back(hairIt->second);
        }

        if (costumeIds.empty() || costumeIds.size() != hairIds.size()) return false;
        return true;
    }

    bool TryGetSolisDefaultLiveIds(void* characterIds, void** safeCostumeIds, void** safeHairIds) {
        if (!characterIds || !safeCostumeIds || !safeHairIds) return false;

        std::vector<std::string> costumeIds{};
        std::vector<std::string> hairIds{};
        if (!GetSolisDefaultLiveIdVectors(characterIds, costumeIds, hairIds)) return false;

        auto newCostumeIds = NewSolisStringList(costumeIds);
        auto newHairIds = NewSolisStringList(hairIds);
        if (!newCostumeIds || !newHairIds) return false;

        *safeCostumeIds = newCostumeIds;
        *safeHairIds = newHairIds;
        Log::DebugFmt("TryGetSolisDefaultLiveIds replaced %d costume/hair ids", static_cast<int>(costumeIds.size()));
        return true;
    }

    bool ReplaceSolisRepeatedStringField(void* field, const std::vector<std::string>& ids) {
        if (!field || ids.empty()) return false;

        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(field);
        auto count = editor.get_Count();
        if (count == static_cast<int>(ids.size())) {
            for (int i = 0; i < count; ++i) {
                editor.set_Item(i, Il2cppString::New(ids[i]));
            }
            return true;
        }
        if (count == 0) {
            for (const auto& id : ids) {
                editor.Add(Il2cppString::New(id));
            }
            return editor.get_Count() == static_cast<int>(ids.size());
        }

        Log::DebugFmt("ReplaceSolisRepeatedStringField skipped: field count %d, replacement count %d",
                      count, static_cast<int>(ids.size()));
        return false;
    }

    bool ForceReplaceSolisRepeatedStringField(void* field, const std::vector<std::string>& ids) {
        if (!field || ids.empty()) return false;

        auto fieldKlass = Il2cppUtils::get_class_from_instance(field);
        auto clearMtd = fieldKlass ? Il2cppUtils::il2cpp_class_get_method_from_name(fieldKlass, "Clear", 0) : nullptr;
        if (!clearMtd) {
            Log::Error("ForceReplaceSolisRepeatedStringField failed: Clear missing");
            return false;
        }

        auto clear = reinterpret_cast<void (*)(void*, void*)>(clearMtd->methodPointer);
        clear(field, clearMtd);

        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(field);
        for (const auto& id : ids) {
            if (!id.empty()) editor.Add(Il2cppString::New(id));
        }
        return editor.get_Count() == static_cast<int>(ids.size());
    }

    std::string SolisRepeatedStringFieldToLogString(void* field) {
        if (!field) return "";

        std::string ret{};
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(field);
        auto count = editor.get_Count();
        for (int i = 0; i < count; ++i) {
            auto item = editor.get_Item(i);
            if (!item) continue;
            if (!ret.empty()) ret += ",";
            ret += item->ToString();
        }
        return ret;
    }

    std::string GetSolisApiSafeShootingCharacterId(const std::string& characterId) {
        if (characterId == "char-mna" || characterId == "char-mng") {
            return "char-ktn";
        }
        return characterId;
    }

    bool ReplaceSolisApiUnsafeShootingCharacterIds(void* characterIds) {
        if (!characterIds) return false;

        bool replaced = false;
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(characterIds);
        auto count = editor.get_Count();
        for (int i = 0; i < count; ++i) {
            auto characterId = editor.get_Item(i);
            if (!characterId) continue;

            auto oldId = characterId->ToString();
            auto safeId = GetSolisApiSafeShootingCharacterId(oldId);
            if (oldId == safeId) continue;

            editor.set_Item(i, Il2cppString::New(safeId));
            replaced = true;
            Log::DebugFmt("ReplaceSolisApiUnsafeShootingCharacterIds: %s -> %s",
                          oldId.c_str(), safeId.c_str());
        }
        return replaced;
    }

    bool ReplaceSolisApiUnsafeShootingCharacterId(void* request, UnityResolve::Method* get_CharacterId,
                                                  UnityResolve::Method* set_CharacterId) {
        if (!request || !get_CharacterId || !set_CharacterId) return false;

        auto characterId = get_CharacterId->Invoke<Il2cppString*>(request);
        if (!characterId) return false;

        auto oldId = characterId->ToString();
        auto safeId = GetSolisApiSafeShootingCharacterId(oldId);
        if (oldId == safeId) return false;

        set_CharacterId->Invoke<void>(request, Il2cppString::New(safeId));
        Log::DebugFmt("ReplaceSolisApiUnsafeShootingCharacterId: %s -> %s",
                      oldId.c_str(), safeId.c_str());
        return true;
    }

    bool ReplaceSolisApiUnsafePhotoCreateShootingParamCharacterIds(void* createShootingParams) {
        if (!createShootingParams) return false;

        static auto get_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCreateShootingParam", "get_MainCharacterId");
        static auto set_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCreateShootingParam", "set_MainCharacterId");
        if (!get_MainCharacterId || !set_MainCharacterId) return false;

        bool replaced = false;
        Il2cppUtils::Tools::CSListEditor<void*> editor(createShootingParams);
        auto count = editor.get_Count();
        for (int i = 0; i < count; ++i) {
            auto param = editor.get_Item(i);
            if (!param) continue;
            replaced |= ReplaceSolisApiUnsafeShootingCharacterId(param, get_MainCharacterId, set_MainCharacterId);
        }
        return replaced;
    }

    bool ReplaceSolisCheckShootingRequestIds(void* characterIds, void* costumeIds, void* hairIds) {
        std::vector<std::string> safeCostumeIds{};
        std::vector<std::string> safeHairIds{};
        if (!GetSolisDefaultLiveIdVectors(characterIds, safeCostumeIds, safeHairIds)) return false;

        Il2cppUtils::Tools::CSListEditor<Il2cppString*> characterEditor(characterIds);
        if (characterEditor.get_Count() <= 0 || !characterEditor.get_Item(0) ||
            safeCostumeIds.empty() || safeHairIds.empty()) {
            return false;
        }

        std::vector<std::string> safeCharacterIds{characterEditor.get_Item(0)->ToString()};
        std::vector<std::string> oneSafeCostumeId{safeCostumeIds[0]};
        std::vector<std::string> oneSafeHairId{safeHairIds[0]};
        auto replacedCharacter = ForceReplaceSolisRepeatedStringField(characterIds, safeCharacterIds);
        auto replacedCostume = ForceReplaceSolisRepeatedStringField(costumeIds, oneSafeCostumeId);
        auto replacedHair = ForceReplaceSolisRepeatedStringField(hairIds, oneSafeHairId);
        Log::DebugFmt("ReplaceSolisCheckShootingRequestIds result: character=%d costume=%d hair=%d count=%d",
                      replacedCharacter, replacedCostume, replacedHair, static_cast<int>(oneSafeCostumeId.size()));
        return replacedCharacter && replacedCostume && replacedHair;
    }

    bool ReplaceSolisPhotoCheckConditionIds(void* request) {
        if (!request) return false;

        static auto get_ActionType = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "PhotoCheckShootingRequest", "get_ActionType");
        static auto set_PhotoActivityId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCheckShootingRequest", "set_PhotoActivityId");
        static auto set_PhotoMusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                              "PhotoCheckShootingRequest", "set_PhotoMusicId");
        static auto set_PhotoStageId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                              "PhotoCheckShootingRequest", "set_PhotoStageId");
        if (!get_ActionType || !set_PhotoActivityId || !set_PhotoMusicId || !set_PhotoStageId) return false;

        auto actionType = get_ActionType->Invoke<int>(request);
        if (actionType == 1 || actionType == 2) {
            set_PhotoActivityId->Invoke<void>(request, Il2cppString::New("photo_activity-studio-00"));
            set_PhotoMusicId->Invoke<void>(request, Il2cppString::New(""));
            set_PhotoStageId->Invoke<void>(request, Il2cppString::New(""));
            Log::Debug("ReplaceSolisPhotoCheckConditionIds: activity check uses photo_activity-studio-00");
            return true;
        }
        if (actionType != 3) return false;  // PhotoShootingActionType_Quest

        set_PhotoActivityId->Invoke<void>(request, Il2cppString::New(""));
        set_PhotoMusicId->Invoke<void>(request, Il2cppString::New("music-hsm-001"));
        set_PhotoStageId->Invoke<void>(request, Il2cppString::New("stage-live-hall-00-00"));
        Log::Debug("ReplaceSolisPhotoCheckConditionIds: quest check uses music-hsm-001/stage-live-hall-00-00");
        return true;
    }

    void NormalizeSolisReplacementCount(std::vector<std::string>& ids, int targetCount) {
        if (targetCount <= 0 || ids.empty()) return;
        if (static_cast<int>(ids.size()) == targetCount) return;

        std::vector<std::string> normalized{};
        normalized.reserve(targetCount);
        for (int i = 0; i < targetCount; ++i) {
            normalized.emplace_back(ids[i % ids.size()]);
        }
        ids = std::move(normalized);
    }

    bool ReplaceSolisSingleShootingRequestIds(void* request, Il2cppString* characterId,
                                              UnityResolve::Method* set_CostumeId,
                                              UnityResolve::Method* set_HairId) {
        if (!request || !characterId || !set_CostumeId || !set_HairId) return false;

        auto characterIds = NewSolisStringList({characterId->ToString()});
        std::vector<std::string> safeCostumeIds{};
        std::vector<std::string> safeHairIds{};
        if (!GetSolisDefaultLiveIdVectors(characterIds, safeCostumeIds, safeHairIds) ||
            safeCostumeIds.empty() || safeHairIds.empty()) {
            return false;
        }

        set_CostumeId->Invoke<void>(request, Il2cppString::New(safeCostumeIds[0]));
        set_HairId->Invoke<void>(request, Il2cppString::New(safeHairIds[0]));
        Log::DebugFmt("ReplaceSolisSingleShootingRequestIds result: costume=%s hair=%s",
                      safeCostumeIds[0].c_str(), safeHairIds[0].c_str());
        return true;
    }

    Il2cppString* GetFirstSolisStringFromList(void* list) {
        if (!list) return nullptr;
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(list);
        if (editor.get_Count() <= 0) return nullptr;
        return editor.get_Item(0);
    }

    bool SolisStringListContains(void* list, const std::string& id) {
        if (!list || id.empty()) return false;
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> editor(list);
        for (auto item : editor) {
            if (item && item->ToString() == id) return true;
        }
        return false;
    }

    bool SolisStringListContainsAny(void* list, void* ids) {
        if (!list || !ids) return false;
        Il2cppUtils::Tools::CSListEditor<Il2cppString*> idEditor(ids);
        for (auto id : idEditor) {
            if (id && SolisStringListContains(list, id->ToString())) return true;
        }
        return false;
    }

    std::string GetSolisSafePhotoExpressionId(void* characterIds, const std::string& currentExpressionId = "") {
        if (!characterIds) return currentExpressionId;

        Il2cppUtils::Tools::CSListEditor<Il2cppString*> characterEditor(characterIds);
        auto characterCount = characterEditor.get_Count();
        if (characterCount <= 0) return currentExpressionId;

        static auto get_PhotoExpressionMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                       "MasterManager", "get_PhotoExpressionMaster");
        static auto PhotoExpressionMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                       "Solis.Common.Master",
                                                                                       "PhotoExpressionMaster",
                                                                                       "GetAllWithSortByKey",
                                                                                       {"Solis.Common.Master.PhotoExpressionSortType"});
        static auto PhotoExpression_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                    "PhotoExpression", "get_Id");
        static auto PhotoExpression_get_CostumeTypeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                "Solis.Common.Proto.Master",
                                                                                "PhotoExpression", "get_CostumeTypeIds");
        static auto PhotoExpression_get_ForceCostumeTypeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                   "Solis.Common.Proto.Master",
                                                                                   "PhotoExpression", "get_ForceCostumeTypeId");
        static auto PhotoExpression_get_ImpossibleShootingCharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                                "Solis.Common.Proto.Master",
                                                                                                "PhotoExpression",
                                                                                                "get_ImpossibleShootingCharacterIds");
        static auto PhotoExpression_GetMaxCharacterAmount = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                   "Solis.Common.Proto.Master",
                                                                                   "PhotoExpression",
                                                                                   "GetMaxCharacterAmount");
        if (!get_PhotoExpressionMaster || !PhotoExpressionMaster_GetAllWithSortByKey ||
            !PhotoExpression_get_Id || !PhotoExpression_get_ImpossibleShootingCharacterIds ||
            !PhotoExpression_GetMaxCharacterAmount) {
            Log::Error("GetSolisSafePhotoExpressionId failed: methods missing");
            return currentExpressionId;
        }

        auto master = get_PhotoExpressionMaster->Invoke<void*>(nullptr);
        auto expressionList = master ? PhotoExpressionMaster_GetAllWithSortByKey->Invoke<void*>(master, 0x0) : nullptr;
        if (!expressionList) return currentExpressionId;

        auto isExpressionSafe = [&](void* expression, bool requireNoCostumeType) -> bool {
            if (!expression) return false;
            auto id = PhotoExpression_get_Id->Invoke<Il2cppString*>(expression);
            if (!id || id->ToString().empty()) return false;

            auto maxCharacterAmount = PhotoExpression_GetMaxCharacterAmount->Invoke<int>(expression);
            if (maxCharacterAmount > 0 && characterCount > maxCharacterAmount) return false;

            auto impossibleCharacterIds = PhotoExpression_get_ImpossibleShootingCharacterIds->Invoke<void*>(expression);
            if (SolisStringListContainsAny(impossibleCharacterIds, characterIds)) return false;

            if (requireNoCostumeType && PhotoExpression_get_CostumeTypeIds && PhotoExpression_get_ForceCostumeTypeId) {
                auto costumeTypeIds = PhotoExpression_get_CostumeTypeIds->Invoke<void*>(expression);
                Il2cppUtils::Tools::CSListEditor<Il2cppString*> costumeTypeEditor(costumeTypeIds);
                auto forceCostumeTypeId = PhotoExpression_get_ForceCostumeTypeId->Invoke<Il2cppString*>(expression);
                if (costumeTypeEditor.get_Count() > 0) return false;
                if (forceCostumeTypeId && !forceCostumeTypeId->ToString().empty()) return false;
            }

            return true;
        };

        Il2cppUtils::Tools::CSListEditor<void*> expressionEditor(expressionList);
        if (!currentExpressionId.empty()) {
            for (auto expression : expressionEditor) {
                auto id = expression && PhotoExpression_get_Id ? PhotoExpression_get_Id->Invoke<Il2cppString*>(expression) : nullptr;
                if (id && id->ToString() == currentExpressionId && isExpressionSafe(expression, true)) {
                    return currentExpressionId;
                }
            }
        }

        for (auto expression : expressionEditor) {
            if (!isExpressionSafe(expression, true)) continue;
            auto id = PhotoExpression_get_Id->Invoke<Il2cppString*>(expression);
            return id->ToString();
        }

        for (auto expression : expressionEditor) {
            if (!isExpressionSafe(expression, false)) continue;
            auto id = PhotoExpression_get_Id->Invoke<Il2cppString*>(expression);
            return id->ToString();
        }

        return currentExpressionId;
    }

    bool GetSolisSafeIdsFromCostumeHairFields(void* costumeIds, void* hairIds,
                                              std::vector<std::string>& safeCostumeIds,
                                              std::vector<std::string>& safeHairIds) {
        if (!costumeIds && !hairIds) return false;

        static auto get_CharacterMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                 "MasterManager", "get_CharacterMaster");
        static auto get_CostumeMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                               "MasterManager", "get_CostumeMaster");
        static auto get_HairMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                            "MasterManager", "get_HairMaster");
        static auto CharacterMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                 "Solis.Common.Master",
                                                                                 "CharacterMaster", "GetAllWithSortByKey",
                                                                                 {"Solis.Common.Master.CharacterSortType"});
        static auto CostumeMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                               "Solis.Common.Master",
                                                                               "CostumeMaster", "GetAllWithSortByKey",
                                                                               {"Solis.Common.Master.CostumeSortType"});
        static auto HairMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                            "Solis.Common.Master",
                                                                            "HairMaster", "GetAllWithSortByKey",
                                                                            {"Solis.Common.Master.HairSortType"});
        static auto Character_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                              "Character", "get_Id");
        static auto Character_get_DefaultLiveCostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                "Solis.Common.Proto.Master",
                                                                                "Character", "get_DefaultLiveCostumeId");
        static auto Costume_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                            "Costume", "get_Id");
        static auto Costume_get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                     "Costume", "get_CharacterId");
        static auto Costume_get_DefaultHairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                       "Solis.Common.Proto.Master",
                                                                       "Costume", "get_DefaultHairId");
        static auto Hair_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                         "Hair", "get_Id");
        static auto Hair_get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                  "Hair", "get_CharacterId");
        if (!get_CharacterMaster || !get_CostumeMaster || !get_HairMaster || !CharacterMaster_GetAllWithSortByKey ||
            !CostumeMaster_GetAllWithSortByKey || !HairMaster_GetAllWithSortByKey || !Character_get_Id ||
            !Character_get_DefaultLiveCostumeId || !Costume_get_Id || !Costume_get_CharacterId ||
            !Costume_get_DefaultHairId || !Hair_get_Id || !Hair_get_CharacterId) {
            Log::Error("GetSolisSafeIdsFromCostumeHairFields failed: master methods missing");
            return false;
        }

        static std::unordered_map<std::string, std::string> defaultLiveCostumeByCharacter{};
        static std::unordered_map<std::string, std::string> defaultHairByCostume{};
        static std::unordered_map<std::string, std::string> characterByCostume{};
        static std::unordered_map<std::string, std::string> characterByHair{};

        auto characterMaster = get_CharacterMaster->Invoke<void*>(nullptr);
        auto costumeMaster = get_CostumeMaster->Invoke<void*>(nullptr);
        auto hairMaster = get_HairMaster->Invoke<void*>(nullptr);
        if (!characterMaster || !costumeMaster || !hairMaster) return false;

        if (defaultLiveCostumeByCharacter.empty()) {
            auto characterList = CharacterMaster_GetAllWithSortByKey->Invoke<void*>(characterMaster, 0x0);
            if (!characterList) return false;
            Il2cppUtils::Tools::CSListEditor<void*> editor(characterList);
            for (auto character : editor) {
                if (!character) continue;
                auto characterId = Character_get_Id->Invoke<Il2cppString*>(character);
                auto costumeId = Character_get_DefaultLiveCostumeId->Invoke<Il2cppString*>(character);
                if (!characterId || !costumeId) continue;
                auto characterIdStr = characterId->ToString();
                auto costumeIdStr = costumeId->ToString();
                if (!characterIdStr.empty() && !costumeIdStr.empty()) defaultLiveCostumeByCharacter[characterIdStr] = costumeIdStr;
            }
        }
        if (defaultHairByCostume.empty() || characterByCostume.empty()) {
            auto costumeList = CostumeMaster_GetAllWithSortByKey->Invoke<void*>(costumeMaster, 0x0);
            if (!costumeList) return false;
            Il2cppUtils::Tools::CSListEditor<void*> editor(costumeList);
            for (auto costume : editor) {
                if (!costume) continue;
                auto costumeId = Costume_get_Id->Invoke<Il2cppString*>(costume);
                auto characterId = Costume_get_CharacterId->Invoke<Il2cppString*>(costume);
                auto hairId = Costume_get_DefaultHairId->Invoke<Il2cppString*>(costume);
                if (!costumeId) continue;
                auto costumeIdStr = costumeId->ToString();
                if (characterId) {
                    auto characterIdStr = characterId->ToString();
                    if (!costumeIdStr.empty() && !characterIdStr.empty()) characterByCostume[costumeIdStr] = characterIdStr;
                }
                if (hairId) {
                    auto hairIdStr = hairId->ToString();
                    if (!costumeIdStr.empty() && !hairIdStr.empty()) defaultHairByCostume[costumeIdStr] = hairIdStr;
                }
            }
        }
        if (characterByHair.empty()) {
            auto hairList = HairMaster_GetAllWithSortByKey->Invoke<void*>(hairMaster, 0x0);
            if (!hairList) return false;
            Il2cppUtils::Tools::CSListEditor<void*> editor(hairList);
            for (auto hair : editor) {
                if (!hair) continue;
                auto hairId = Hair_get_Id->Invoke<Il2cppString*>(hair);
                auto characterId = Hair_get_CharacterId->Invoke<Il2cppString*>(hair);
                if (!hairId || !characterId) continue;
                auto hairIdStr = hairId->ToString();
                auto characterIdStr = characterId->ToString();
                if (!hairIdStr.empty() && !characterIdStr.empty()) characterByHair[hairIdStr] = characterIdStr;
            }
        }

        auto appendSafeForCharacter = [&](const std::string& characterId) {
            auto costumeIt = defaultLiveCostumeByCharacter.find(characterId);
            if (costumeIt == defaultLiveCostumeByCharacter.end()) return;
            auto hairIt = defaultHairByCostume.find(costumeIt->second);
            if (hairIt == defaultHairByCostume.end()) return;
            safeCostumeIds.emplace_back(costumeIt->second);
            safeHairIds.emplace_back(hairIt->second);
        };

        safeCostumeIds.clear();
        safeHairIds.clear();
        if (costumeIds) {
            Il2cppUtils::Tools::CSListEditor<Il2cppString*> costumeEditor(costumeIds);
            for (auto costumeId : costumeEditor) {
                if (!costumeId) continue;
                auto characterIt = characterByCostume.find(costumeId->ToString());
                if (characterIt != characterByCostume.end()) appendSafeForCharacter(characterIt->second);
            }
        }
        if (safeCostumeIds.empty() && hairIds) {
            Il2cppUtils::Tools::CSListEditor<Il2cppString*> hairEditor(hairIds);
            for (auto hairId : hairEditor) {
                if (!hairId) continue;
                auto characterIt = characterByHair.find(hairId->ToString());
                if (characterIt != characterByHair.end()) appendSafeForCharacter(characterIt->second);
            }
        }

        return !safeCostumeIds.empty() && safeCostumeIds.size() == safeHairIds.size();
    }

    bool ReplaceSolisCostumeHairRequestIds(void* costumeIds, void* hairIds) {
        std::vector<std::string> safeCostumeIds{};
        std::vector<std::string> safeHairIds{};
        if (!GetSolisSafeIdsFromCostumeHairFields(costumeIds, hairIds, safeCostumeIds, safeHairIds)) return false;

        safeCostumeIds = {safeCostumeIds[0]};
        safeHairIds = {safeHairIds[0]};

        auto replacedCostume = ForceReplaceSolisRepeatedStringField(costumeIds, safeCostumeIds);
        auto replacedHair = ForceReplaceSolisRepeatedStringField(hairIds, safeHairIds);
        Log::DebugFmt("ReplaceSolisCostumeHairRequestIds result: costume=%d hair=%d costumeCount=%d hairCount=%d",
                      replacedCostume, replacedHair, static_cast<int>(safeCostumeIds.size()),
                      static_cast<int>(safeHairIds.size()));
        return replacedCostume && replacedHair;
    }

    bool ReplaceSolisCostumeSetRequestIds(void* request,
                                          UnityResolve::Method* get_CostumeId,
                                          UnityResolve::Method* get_HairId,
                                          UnityResolve::Method* set_CostumeId,
                                          UnityResolve::Method* set_HairId) {
        if (!request || !get_CostumeId || !get_HairId || !set_CostumeId || !set_HairId) return false;

        auto costumeId = get_CostumeId->Invoke<Il2cppString*>(request);
        auto hairId = get_HairId->Invoke<Il2cppString*>(request);
        std::vector<std::string> requestedCostumeIds{costumeId ? costumeId->ToString() : ""};
        std::vector<std::string> requestedHairIds{hairId ? hairId->ToString() : ""};
        auto costumeIds = NewSolisStringList(requestedCostumeIds);
        auto hairIds = NewSolisStringList(requestedHairIds);

        std::vector<std::string> safeCostumeIds{};
        std::vector<std::string> safeHairIds{};
        if (!GetSolisSafeIdsFromCostumeHairFields(costumeIds, hairIds, safeCostumeIds, safeHairIds)) return false;
        if (safeCostumeIds.empty() || safeHairIds.empty()) return false;

        set_CostumeId->Invoke<void>(request, Il2cppString::New(safeCostumeIds[0]));
        set_HairId->Invoke<void>(request, Il2cppString::New(safeHairIds[0]));
        Log::DebugFmt("ReplaceSolisCostumeSetRequestIds: %s/%s -> %s/%s",
                      requestedCostumeIds[0].c_str(), requestedHairIds[0].c_str(),
                      safeCostumeIds[0].c_str(), safeHairIds[0].c_str());
        return true;
    }

    bool ReplaceSolisExpressionShootingRequestIds(void* request,
                                                  UnityResolve::Method* get_PhotoExpressionId,
                                                  UnityResolve::Method* set_PhotoExpressionId,
                                                  UnityResolve::Method* get_CharacterId,
                                                  UnityResolve::Method* set_CharacterId,
                                                  UnityResolve::Method* set_CostumeId,
                                                  UnityResolve::Method* set_HairId,
                                                  UnityResolve::Method* get_CharacterIds,
                                                  UnityResolve::Method* get_CostumeIds,
                                                  UnityResolve::Method* get_HairIds) {
        if (!request) return false;

        auto replacedSingle = false;
        auto replacedRepeated = false;
        void* characterIds = nullptr;
        if (get_CharacterIds && get_CostumeIds && get_HairIds) {
            characterIds = get_CharacterIds->Invoke<void*>(request);
            auto costumeIds = get_CostumeIds->Invoke<void*>(request);
            auto hairIds = get_HairIds->Invoke<void*>(request);
            replacedRepeated = ReplaceSolisCheckShootingRequestIds(characterIds, costumeIds, hairIds);
            if (replacedRepeated && set_CharacterId && set_CostumeId && set_HairId) {
                auto characterId = GetFirstSolisStringFromList(characterIds);
                auto costumeId = GetFirstSolisStringFromList(costumeIds);
                auto hairId = GetFirstSolisStringFromList(hairIds);
                if (characterId && costumeId && hairId) {
                    set_CharacterId->Invoke<void>(request, characterId);
                    set_CostumeId->Invoke<void>(request, costumeId);
                    set_HairId->Invoke<void>(request, hairId);
                    replacedSingle = true;
                }
            }
        }
        if (!replacedSingle && get_CharacterId && set_CostumeId && set_HairId) {
            replacedSingle = ReplaceSolisSingleShootingRequestIds(
                    request, get_CharacterId->Invoke<Il2cppString*>(request), set_CostumeId, set_HairId);
        }

        auto replacedExpression = false;
        if (get_PhotoExpressionId && set_PhotoExpressionId && characterIds) {
            auto currentExpressionId = get_PhotoExpressionId->Invoke<Il2cppString*>(request);
            auto safeExpressionId = GetSolisSafePhotoExpressionId(
                    characterIds, currentExpressionId ? currentExpressionId->ToString() : "");
            if (!safeExpressionId.empty() &&
                (!currentExpressionId || currentExpressionId->ToString() != safeExpressionId)) {
                set_PhotoExpressionId->Invoke<void>(request, Il2cppString::New(safeExpressionId));
                replacedExpression = true;
                Log::DebugFmt("ReplaceSolisExpressionShootingRequestIds expression: %s -> %s",
                              currentExpressionId ? currentExpressionId->ToString().c_str() : "",
                              safeExpressionId.c_str());
            }
        }

        Log::DebugFmt("ReplaceSolisExpressionShootingRequestIds result: single=%d repeated=%d expression=%d",
                      replacedSingle, replacedRepeated, replacedExpression);
        return replacedSingle || replacedRepeated || replacedExpression;
    }

    void* GetEmptySolisPhotoPoseTypes() {
        static void* emptyPoseTypes = nullptr;
        if (emptyPoseTypes) return emptyPoseTypes;

        static auto RepeatedField_PhotoPoseType_klass = Il2cppUtils::get_system_class_from_reflection_type_str(
            "Google.Protobuf.Collections.RepeatedField`1[Solis.Common.Proto.PhotoPoseType]");
        static auto RepeatedField_PhotoPoseType_ctor_mtd = RepeatedField_PhotoPoseType_klass ?
                Il2cppUtils::il2cpp_class_get_method_from_name(RepeatedField_PhotoPoseType_klass, ".ctor", 0) : nullptr;
        static auto RepeatedField_PhotoPoseType_ctor = RepeatedField_PhotoPoseType_ctor_mtd ?
                reinterpret_cast<void (*)(void*, void*)>(RepeatedField_PhotoPoseType_ctor_mtd->methodPointer) : nullptr;
        if (!RepeatedField_PhotoPoseType_klass || !RepeatedField_PhotoPoseType_ctor) {
            Log::Error("GetEmptySolisPhotoPoseTypes failed: RepeatedField<PhotoPoseType> missing");
            return nullptr;
        }

        emptyPoseTypes = UnityResolve::Invoke<void*>("il2cpp_object_new", RepeatedField_PhotoPoseType_klass);
        RepeatedField_PhotoPoseType_ctor(emptyPoseTypes, RepeatedField_PhotoPoseType_ctor_mtd);
        return emptyPoseTypes;
    }

    DEFINE_HOOK(void, LiveUtility_LoadLiveScene_FullSkip, (void* data, bool isFullSkip, void* method)) {
        if (Config::skipLiveToResult && data && LiveUtility_LoadLiveResult_Call) {
            Log::InfoFmt("LiveSkip: skip live scene and load result, isFullSkip=%d", isFullSkip ? 1 : 0);
            LiveUtility_LoadLiveResult_Call(data, nullptr);
            return;
        }
        return LiveUtility_LoadLiveScene_FullSkip_Orig(data, isFullSkip, method);
    }
    DEFINE_HOOK(void*, Solis_Photo_CheckShootingAsync, (int actionType, Il2cppString* photoActivityId,
        Il2cppString* photoMusicId, Il2cppString* photoStageId, void* characterIds, void* costumeIds,
        void* hairIds, void* ct, void* callOption, void* errorHandler, Il2cppString* requestIdForResponseCache,
        void* mtd)) {
        if (Config::unlockAllLive) {
            Log::DebugFmt("Photo.CheckShooting args: action=%d activity=%s music=%s stage=%s characters=%s costumes=%s hairs=%s",
                          actionType,
                          photoActivityId ? photoActivityId->ToString().c_str() : "",
                          photoMusicId ? photoMusicId->ToString().c_str() : "",
                          photoStageId ? photoStageId->ToString().c_str() : "",
                          SolisRepeatedStringFieldToLogString(characterIds).c_str(),
                          SolisRepeatedStringFieldToLogString(costumeIds).c_str(),
                          SolisRepeatedStringFieldToLogString(hairIds).c_str());
        }
        if (Config::unlockAllPhotoPose) {
            ReplaceSolisApiUnsafeShootingCharacterIds(characterIds);
        }
        if (Config::unlockAllLiveCostume) {
            void* safeCostumeIds = nullptr;
            void* safeHairIds = nullptr;
            if (TryGetSolisDefaultLiveIds(characterIds, &safeCostumeIds, &safeHairIds)) {
                costumeIds = safeCostumeIds;
                hairIds = safeHairIds;
            }
        }
        return Solis_Photo_CheckShootingAsync_Orig(actionType, photoActivityId, photoMusicId, photoStageId,
                                                   characterIds, costumeIds, hairIds, ct, callOption, errorHandler,
                                                   requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckShootingAsync_Request, (void* request, void* ct, void* callOption,
        void* errorHandler, Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllLive && request) {
            static auto get_ActionType = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckShootingRequest", "get_ActionType");
            static auto get_PhotoActivityId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCheckShootingRequest", "get_PhotoActivityId");
            static auto get_PhotoMusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_PhotoMusicId");
            static auto get_PhotoStageId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_PhotoStageId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckShootingRequest", "get_HairIds");
            auto activityId = get_PhotoActivityId ? get_PhotoActivityId->Invoke<Il2cppString*>(request) : nullptr;
            auto musicId = get_PhotoMusicId ? get_PhotoMusicId->Invoke<Il2cppString*>(request) : nullptr;
            auto stageId = get_PhotoStageId ? get_PhotoStageId->Invoke<Il2cppString*>(request) : nullptr;
            Log::DebugFmt("Photo.CheckShooting request: action=%d activity=%s music=%s stage=%s characters=%s costumes=%s hairs=%s",
                          get_ActionType ? get_ActionType->Invoke<int>(request) : -1,
                          activityId ? activityId->ToString().c_str() : "",
                          musicId ? musicId->ToString().c_str() : "",
                          stageId ? stageId->ToString().c_str() : "",
                          get_CharacterIds ? SolisRepeatedStringFieldToLogString(get_CharacterIds->Invoke<void*>(request)).c_str() : "",
                          get_CostumeIds ? SolisRepeatedStringFieldToLogString(get_CostumeIds->Invoke<void*>(request)).c_str() : "",
                          get_HairIds ? SolisRepeatedStringFieldToLogString(get_HairIds->Invoke<void*>(request)).c_str() : "");
            auto replacedCondition = ReplaceSolisPhotoCheckConditionIds(request);
            if (replacedCondition && get_ActionType && get_ActionType->Invoke<int>(request) != 3 &&
                get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_CharacterIds");
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckShootingRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Photo_CheckShootingAsync_Request_Orig(request, ct, callOption, errorHandler,
                                                           requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckSpecialShootingAsync, (int actionType, Il2cppString* specialPhotoShootingId,
        Il2cppString* musicId, Il2cppString* stageId, void* characterIds, void* costumeIds, void* hairIds,
        void* ct, void* callOption, void* errorHandler, Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllPhotoPose) {
            ReplaceSolisApiUnsafeShootingCharacterIds(characterIds);
        }
        if (Config::unlockAllLiveCostume) {
            void* safeCostumeIds = nullptr;
            void* safeHairIds = nullptr;
            if (TryGetSolisDefaultLiveIds(characterIds, &safeCostumeIds, &safeHairIds)) {
                costumeIds = safeCostumeIds;
                hairIds = safeHairIds;
            }
        }
        return Solis_Photo_CheckSpecialShootingAsync_Orig(actionType, specialPhotoShootingId, musicId, stageId,
                                                          characterIds, costumeIds, hairIds, ct, callOption,
                                                          errorHandler, requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckSpecialShootingAsync_Request, (void* request, void* ct, void* callOption,
        void* errorHandler, Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckSpecialShootingRequest", "get_CharacterIds");
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckSpecialShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckSpecialShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckSpecialShootingRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Photo_CheckSpecialShootingAsync_Request_Orig(request, ct, callOption, errorHandler,
                                                                  requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoContest_CheckShootingAsync, (Il2cppString* photoContestId, int actionType,
        Il2cppString* photoContestActivityId, Il2cppString* photoContestQuestMusicId,
        Il2cppString* photoContestQuestStageId, void* selectedCharacterIds, void* selectedCostumeIds,
        Il2cppString* sectionId, void* selectedHairIds, void* ct, void* callOption, void* errorHandler,
        Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllPhotoPose) {
            ReplaceSolisApiUnsafeShootingCharacterIds(selectedCharacterIds);
        }
        if (Config::unlockAllLiveCostume) {
            void* safeCostumeIds = nullptr;
            void* safeHairIds = nullptr;
            if (TryGetSolisDefaultLiveIds(selectedCharacterIds, &safeCostumeIds, &safeHairIds)) {
                selectedCostumeIds = safeCostumeIds;
                selectedHairIds = safeHairIds;
            }
        }
        return Solis_PhotoContest_CheckShootingAsync_Orig(photoContestId, actionType, photoContestActivityId,
                                                          photoContestQuestMusicId, photoContestQuestStageId,
                                                          selectedCharacterIds, selectedCostumeIds, sectionId,
                                                          selectedHairIds, ct, callOption, errorHandler,
                                                          requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoContest_CheckShootingAsync_Request, (void* request, void* ct, void* callOption,
        void* errorHandler, Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_SelectedCharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoContestCheckShootingRequest", "get_SelectedCharacterIds");
            if (get_SelectedCharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_SelectedCharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_SelectedCharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoContestCheckShootingRequest", "get_SelectedCharacterIds");
            static auto get_SelectedCostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                        "PhotoContestCheckShootingRequest", "get_SelectedCostumeIds");
            static auto get_SelectedHairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoContestCheckShootingRequest", "get_SelectedHairIds");
            if (get_SelectedCharacterIds && get_SelectedCostumeIds && get_SelectedHairIds) {
                ReplaceSolisCheckShootingRequestIds(get_SelectedCharacterIds->Invoke<void*>(request),
                                                    get_SelectedCostumeIds->Invoke<void*>(request),
                                                    get_SelectedHairIds->Invoke<void*>(request));
            }
        }
        return Solis_PhotoContest_CheckShootingAsync_Request_Orig(request, ct, callOption, errorHandler,
                                                                  requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoPanorama_CheckShootingAsync, (Il2cppString* photoMusicId, void* characterIds,
        void* costumeIds, void* hairIds, void* ct, void* callOption, void* errorHandler,
        Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllPhotoPose) {
            ReplaceSolisApiUnsafeShootingCharacterIds(characterIds);
        }
        if (Config::unlockAllLiveCostume) {
            void* safeCostumeIds = nullptr;
            void* safeHairIds = nullptr;
            if (TryGetSolisDefaultLiveIds(characterIds, &safeCostumeIds, &safeHairIds)) {
                costumeIds = safeCostumeIds;
                hairIds = safeHairIds;
            }
        }
        return Solis_PhotoPanorama_CheckShootingAsync_Orig(photoMusicId, characterIds, costumeIds, hairIds, ct,
                                                           callOption, errorHandler, requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoPanorama_CheckShootingAsync_Request, (void* request, void* ct, void* callOption,
        void* errorHandler, Il2cppString* requestIdForResponseCache, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoPanoramaCheckShootingRequest", "get_CharacterIds");
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoPanoramaCheckShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoPanoramaCheckShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoPanoramaCheckShootingRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_PhotoPanorama_CheckShootingAsync_Request_Orig(request, ct, callOption, errorHandler,
                                                                   requestIdForResponseCache, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllLive && request) {
            static auto get_ActionType = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckShootingRequest", "get_ActionType");
            static auto get_PhotoActivityId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCheckShootingRequest", "get_PhotoActivityId");
            static auto get_PhotoMusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_PhotoMusicId");
            static auto get_PhotoStageId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_PhotoStageId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckShootingRequest", "get_HairIds");
            auto activityId = get_PhotoActivityId ? get_PhotoActivityId->Invoke<Il2cppString*>(request) : nullptr;
            auto musicId = get_PhotoMusicId ? get_PhotoMusicId->Invoke<Il2cppString*>(request) : nullptr;
            auto stageId = get_PhotoStageId ? get_PhotoStageId->Invoke<Il2cppString*>(request) : nullptr;
            Log::DebugFmt("Photo.CheckShooting call: action=%d activity=%s music=%s stage=%s characters=%s costumes=%s hairs=%s",
                          get_ActionType ? get_ActionType->Invoke<int>(request) : -1,
                          activityId ? activityId->ToString().c_str() : "",
                          musicId ? musicId->ToString().c_str() : "",
                          stageId ? stageId->ToString().c_str() : "",
                          get_CharacterIds ? SolisRepeatedStringFieldToLogString(get_CharacterIds->Invoke<void*>(request)).c_str() : "",
                          get_CostumeIds ? SolisRepeatedStringFieldToLogString(get_CostumeIds->Invoke<void*>(request)).c_str() : "",
                          get_HairIds ? SolisRepeatedStringFieldToLogString(get_HairIds->Invoke<void*>(request)).c_str() : "");
            auto replacedCondition = ReplaceSolisPhotoCheckConditionIds(request);
            if (replacedCondition && get_ActionType && get_ActionType->Invoke<int>(request) != 3 &&
                get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_CharacterIds");
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckShootingRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Photo_CheckShootingAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckSpecialShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckSpecialShootingRequest", "get_CharacterIds");
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckSpecialShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckSpecialShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckSpecialShootingRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Photo_CheckSpecialShootingAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckExpressionShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCheckExpressionShootingRequest", "get_CharacterId");
            static auto set_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCheckExpressionShootingRequest", "set_CharacterId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckExpressionShootingRequest", "get_CharacterIds");
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_CharacterId, set_CharacterId);
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_PhotoExpressionId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                       "PhotoCheckExpressionShootingRequest",
                                                                       "get_PhotoExpressionId");
            static auto set_PhotoExpressionId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                       "PhotoCheckExpressionShootingRequest",
                                                                       "set_PhotoExpressionId");
            static auto get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCheckExpressionShootingRequest", "get_CharacterId");
            static auto set_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCheckExpressionShootingRequest", "set_CharacterId");
            static auto set_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                               "PhotoCheckExpressionShootingRequest", "set_CostumeId");
            static auto set_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "PhotoCheckExpressionShootingRequest", "set_HairId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCheckExpressionShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCheckExpressionShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCheckExpressionShootingRequest", "get_HairIds");
            ReplaceSolisExpressionShootingRequestIds(request, get_PhotoExpressionId, set_PhotoExpressionId,
                                                     get_CharacterId, set_CharacterId,
                                                     set_CostumeId, set_HairId,
                                                     get_CharacterIds, get_CostumeIds, get_HairIds);
        }
        return Solis_Photo_CheckExpressionShootingAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CreateShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCreateShootingsRequest", "get_MainCharacterId");
            static auto set_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCreateShootingsRequest", "set_MainCharacterId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCreateShootingsRequest", "get_CharacterIds");
            static auto get_CreateShootingParams = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoCreateShootingsRequest",
                                                                          "get_CreateShootingParams");
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_MainCharacterId, set_MainCharacterId);
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
            if (get_CreateShootingParams) {
                ReplaceSolisApiUnsafePhotoCreateShootingParamCharacterIds(get_CreateShootingParams->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCreateShootingsRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCreateShootingsRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCreateShootingsRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Photo_CreateShootingsAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CreateSpecialShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCreateSpecialShootingsRequest", "get_MainCharacterId");
            static auto set_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCreateSpecialShootingsRequest", "set_MainCharacterId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCreateSpecialShootingsRequest", "get_CharacterIds");
            static auto get_CreateShootingParams = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoCreateSpecialShootingsRequest",
                                                                          "get_CreateShootingParams");
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_MainCharacterId, set_MainCharacterId);
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
            if (get_CreateShootingParams) {
                ReplaceSolisApiUnsafePhotoCreateShootingParamCharacterIds(get_CreateShootingParams->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCreateSpecialShootingsRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCreateSpecialShootingsRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCreateSpecialShootingsRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Photo_CreateSpecialShootingsAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CreateExpressionShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCreateExpressionShootingsRequest", "get_MainCharacterId");
            static auto set_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoCreateExpressionShootingsRequest", "set_MainCharacterId");
            static auto get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCreateExpressionShootingsRequest", "get_CharacterId");
            static auto set_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCreateExpressionShootingsRequest", "set_CharacterId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCreateExpressionShootingsRequest", "get_CharacterIds");
            static auto get_CreateShootingParams = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoCreateExpressionShootingsRequest",
                                                                          "get_CreateShootingParams");
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_MainCharacterId, set_MainCharacterId);
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_CharacterId, set_CharacterId);
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
            if (get_CreateShootingParams) {
                ReplaceSolisApiUnsafePhotoCreateShootingParamCharacterIds(get_CreateShootingParams->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_PhotoExpressionId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                       "PhotoCreateExpressionShootingsRequest",
                                                                       "get_PhotoExpressionId");
            static auto set_PhotoExpressionId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                       "PhotoCreateExpressionShootingsRequest",
                                                                       "set_PhotoExpressionId");
            static auto get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCreateExpressionShootingsRequest", "get_CharacterId");
            static auto set_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                 "PhotoCreateExpressionShootingsRequest", "set_CharacterId");
            static auto set_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                               "PhotoCreateExpressionShootingsRequest", "set_CostumeId");
            static auto set_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "PhotoCreateExpressionShootingsRequest", "set_HairId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoCreateExpressionShootingsRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoCreateExpressionShootingsRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoCreateExpressionShootingsRequest", "get_HairIds");
            ReplaceSolisExpressionShootingRequestIds(request, get_PhotoExpressionId, set_PhotoExpressionId,
                                                     get_CharacterId, set_CharacterId,
                                                     set_CostumeId, set_HairId,
                                                     get_CharacterIds, get_CostumeIds, get_HairIds);
        }
        return Solis_Photo_CreateExpressionShootingsAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoContest_CheckShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_SelectedCharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoContestCheckShootingRequest", "get_SelectedCharacterIds");
            if (get_SelectedCharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_SelectedCharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_SelectedCharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoContestCheckShootingRequest", "get_SelectedCharacterIds");
            static auto get_SelectedCostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                        "PhotoContestCheckShootingRequest", "get_SelectedCostumeIds");
            static auto get_SelectedHairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoContestCheckShootingRequest", "get_SelectedHairIds");
            if (get_SelectedCharacterIds && get_SelectedCostumeIds && get_SelectedHairIds) {
                ReplaceSolisCheckShootingRequestIds(get_SelectedCharacterIds->Invoke<void*>(request),
                                                    get_SelectedCostumeIds->Invoke<void*>(request),
                                                    get_SelectedHairIds->Invoke<void*>(request));
            }
        }
        return Solis_PhotoContest_CheckShootingAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoContest_SubmitShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoContestSubmitShootingRequest",
                                                                     "get_MainCharacterId");
            static auto set_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoContestSubmitShootingRequest",
                                                                     "set_MainCharacterId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoContestSubmitShootingRequest",
                                                                  "get_CharacterIds");
            static auto get_SelectedCharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoContestSubmitShootingRequest",
                                                                          "get_SelectedCharacterIds");
            static auto get_CreateShootingParams = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoContestSubmitShootingRequest",
                                                                          "get_CreateShootingParams");
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_MainCharacterId, set_MainCharacterId);
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
            if (get_SelectedCharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_SelectedCharacterIds->Invoke<void*>(request));
            }
            if (get_CreateShootingParams) {
                ReplaceSolisApiUnsafePhotoCreateShootingParamCharacterIds(get_CreateShootingParams->Invoke<void*>(request));
            }
        }
        return Solis_PhotoContest_SubmitShootingAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoPanorama_CheckShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoPanoramaCheckShootingRequest", "get_CharacterIds");
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoPanoramaCheckShootingRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoPanoramaCheckShootingRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoPanoramaCheckShootingRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_PhotoPanorama_CheckShootingAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoPanorama_CreateShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllPhotoPose && request) {
            static auto get_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoPanoramaCreateShootingsRequest", "get_MainCharacterId");
            static auto set_MainCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                     "PhotoPanoramaCreateShootingsRequest", "set_MainCharacterId");
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoPanoramaCreateShootingsRequest", "get_CharacterIds");
            static auto get_CreateShootingParams = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                          "PhotoPanoramaCreateShootingsRequest",
                                                                          "get_CreateShootingParams");
            ReplaceSolisApiUnsafeShootingCharacterId(request, get_MainCharacterId, set_MainCharacterId);
            if (get_CharacterIds) {
                ReplaceSolisApiUnsafeShootingCharacterIds(get_CharacterIds->Invoke<void*>(request));
            }
            if (get_CreateShootingParams) {
                ReplaceSolisApiUnsafePhotoCreateShootingParamCharacterIds(get_CreateShootingParams->Invoke<void*>(request));
            }
        }
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CharacterIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                  "PhotoPanoramaCreateShootingsRequest", "get_CharacterIds");
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "PhotoPanoramaCreateShootingsRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "PhotoPanoramaCreateShootingsRequest", "get_HairIds");
            if (get_CharacterIds && get_CostumeIds && get_HairIds) {
                ReplaceSolisCheckShootingRequestIds(get_CharacterIds->Invoke<void*>(request),
                                                    get_CostumeIds->Invoke<void*>(request),
                                                    get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_PhotoPanorama_CreateShootingsAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Costume_SetCostumeAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                               "CostumeSetRequest", "get_CostumeId");
            static auto get_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "CostumeSetRequest", "get_HairId");
            static auto set_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                               "CostumeSetRequest", "set_CostumeId");
            static auto set_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "CostumeSetRequest", "set_HairId");
            ReplaceSolisCostumeSetRequestIds(request, get_CostumeId, get_HairId, set_CostumeId, set_HairId);
        }
        return Solis_Costume_SetCostumeAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Costume_SetLiveCostumeAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                               "CostumeLiveSetRequest", "get_CostumeId");
            static auto get_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "CostumeLiveSetRequest", "get_HairId");
            static auto set_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                               "CostumeLiveSetRequest", "set_CostumeId");
            static auto set_HairId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                            "CostumeLiveSetRequest", "set_HairId");
            ReplaceSolisCostumeSetRequestIds(request, get_CostumeId, get_HairId, set_CostumeId, set_HairId);
        }
        return Solis_Costume_SetLiveCostumeAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Costume_CheckBulkAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        if (Config::unlockAllLiveCostume && request) {
            static auto get_CostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                                "CostumeCheckBulkRequest", "get_CostumeIds");
            static auto get_HairIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                                             "CostumeCheckBulkRequest", "get_HairIds");
            if (get_CostumeIds && get_HairIds) {
                ReplaceSolisCostumeHairRequestIds(get_CostumeIds->Invoke<void*>(request),
                                                  get_HairIds->Invoke<void*>(request));
            }
        }
        return Solis_Costume_CheckBulkAsync_Call_Orig(self, request, metadata, deadline, ct, mtd);
    }

    struct SolisUniTask {
        void* source;
        int16_t token;
        int16_t padding0;
        int32_t padding1;
    };

    SolisUniTask getSolisCompletedUniTask() {
        SolisUniTask ret{};
        static auto unitask_klass = Il2cppUtils::GetClass("UniTask.dll", "Cysharp.Threading.Tasks", "UniTask");
        static auto CompletedTask_field = unitask_klass ? unitask_klass->Get<UnityResolve::Field>("CompletedTask") : nullptr;
        if (!unitask_klass || !CompletedTask_field) return ret;

        UnityResolve::Invoke<void>("il2cpp_field_static_get_value", CompletedTask_field->address, &ret);
        return ret;
    }

    DEFINE_HOOK(SolisUniTask, Solis_CharacterCostumeScreenPresenter_CheckBulkAsync, (void* self, void* ct, void* mtd)) {
        if (Config::unlockAllLiveCostume) {
            return getSolisCompletedUniTask();
        }
        return Solis_CharacterCostumeScreenPresenter_CheckBulkAsync_Orig(self, ct, mtd);
    }

    DEFINE_HOOK(bool, Solis_Costume_get_IsUserUnavailable, (void* self, void* mtd)) {
        auto ret = Solis_Costume_get_IsUserUnavailable_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume || !self) return ret;
        return false;
    }

    DEFINE_HOOK(bool, Solis_Costume_get_IsDisableAdmin, (void* self, void* mtd)) {
        auto ret = Solis_Costume_get_IsDisableAdmin_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume || !self) return ret;
        return false;
    }

    DEFINE_HOOK(bool, Solis_Hair_get_IsDisableAdmin, (void* self, void* mtd)) {
        auto ret = Solis_Hair_get_IsDisableAdmin_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume || !self) return ret;
        return false;
    }

    DEFINE_HOOK(void*, Solis_Costume_get_ImpossiblePhotoPoseTypes, (void* self, void* mtd)) {
        auto ret = Solis_Costume_get_ImpossiblePhotoPoseTypes_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume) return ret;

        auto emptyPoseTypes = GetEmptySolisPhotoPoseTypes();
        return emptyPoseTypes ? emptyPoseTypes : ret;
    }

    DEFINE_HOOK(void*, Solis_Hair_get_ImpossiblePhotoPoseTypes, (void* self, void* mtd)) {
        auto ret = Solis_Hair_get_ImpossiblePhotoPoseTypes_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume) return ret;

        auto emptyPoseTypes = GetEmptySolisPhotoPoseTypes();
        return emptyPoseTypes ? emptyPoseTypes : ret;
    }

    DEFINE_HOOK(void*, Solis_ActorCostume_get_SdHair, (void* self, void* mtd)) {
        auto ret = Solis_ActorCostume_get_SdHair_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume || !self) return ret;
        return nullptr;
    }

    DEFINE_HOOK(void*, Solis_ActorCostume_GetCurrentIntersectedPoseTypes, (void* self, void* mtd)) {
        auto ret = Solis_ActorCostume_GetCurrentIntersectedPoseTypes_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume || !self) return ret;

        static auto ActorCostume_get_Costume = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Data",
                                                                      "ActorCostume", "get_Costume");
        static auto Costume_GetPossiblePoseTypes = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                          "Solis.Common.Proto.Master",
                                                                          "Costume", "GetPossiblePoseTypes");
        if (!ActorCostume_get_Costume || !Costume_GetPossiblePoseTypes) return ret;

        auto costume = ActorCostume_get_Costume->Invoke<void*>(self);
        if (!costume) return ret;

        auto poseTypes = Costume_GetPossiblePoseTypes->Invoke<void*>(costume);
        return poseTypes ? poseTypes : ret;
    }

    DEFINE_HOOK(void*, Solis_ActorCostume_GetExpressionIntersectedPoseTypes,
        (void* self, void* expression, void* mtd)) {
        auto ret = Solis_ActorCostume_GetExpressionIntersectedPoseTypes_Orig(self, expression, mtd);
        if (!Config::unlockAllLiveCostume || !expression) return ret;

        static auto PhotoExpression_GetPossiblePoseTypes = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                  "Solis.Common.Proto.Master",
                                                                                  "PhotoExpression",
                                                                                  "GetPossiblePoseTypes");
        if (!PhotoExpression_GetPossiblePoseTypes) return ret;

        auto poseTypes = PhotoExpression_GetPossiblePoseTypes->Invoke<void*>(expression);
        return poseTypes ? poseTypes : ret;
    }

    DEFINE_HOOK(void*, Solis_UserCostumeCollection_FindBy, (void* self, void* predicate, void* mtd)) {
        auto ret = Solis_UserCostumeCollection_FindBy_Orig(self, predicate, mtd);
        if (!Config::unlockAllLiveCostume) return ret;

        auto this_klass = Il2cppUtils::get_class_from_instance(self);
        if (!this_klass || std::string(this_klass->name) != "UserCostumeCollection") return ret;

        static auto UserCostumeCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                        "UserCostumeCollection");
        static auto UserCostumeCollection_GetAllList_mtd = UserCostumeCollection_klass ?
                Il2cppUtils::il2cpp_class_get_method_from_name(UserCostumeCollection_klass->address, "GetAllList", 1) : nullptr;
        static auto UserCostumeCollection_GetAllList = UserCostumeCollection_GetAllList_mtd ?
                reinterpret_cast<void* (*)(void*, void*)>(UserCostumeCollection_GetAllList_mtd->methodPointer) : nullptr;
        if (!UserCostumeCollection_GetAllList) return ret;

        auto origList = UserCostumeCollection_GetAllList(self, nullptr);
        return AddSolisCostumesToUserCollection(origList);
    }

    DEFINE_HOOK(void*, Solis_UserCostumeCollection_GetAllList, (void* self, void* comparison, void* mtd)) {
        auto ret = Solis_UserCostumeCollection_GetAllList_Orig(self, comparison, mtd);

        auto this_klass = Il2cppUtils::get_class_from_instance(self);
        if (!this_klass) return ret;

        std::string thisKlassName(this_klass->name);
        if (Config::unlockAllLiveCostume) {
            if (thisKlassName == "UserCostumeCollection") return AddSolisCostumesToUserCollection(ret);
            if (thisKlassName == "UserHairCollection") return AddSolisHairsToUserCollection(ret);
        }
        if (Config::unlockAllLive && thisKlassName == "UserMusicCollection") {
            return AddSolisMusicsToUserCollection(ret);
        }
        if (Config::unlockAllPhotoPose && thisKlassName == "UserPhotoPoseCollection") {
            return AddSolisPhotoPosesToUserCollection(ret);
        }
        return ret;
    }

    DEFINE_HOOK(bool, Solis_UserCostumeCollection_Exists, (void* self, Il2cppString* costumeId, void* mtd)) {
        auto ret = Solis_UserCostumeCollection_Exists_Orig(self, costumeId, mtd);
        if (!costumeId) return ret;

        auto this_klass = Il2cppUtils::get_class_from_instance(self);
        if (!this_klass) return ret;

        auto idStr = costumeId->ToString();
        std::string thisKlassName(this_klass->name);
        if (Config::unlockAllLiveCostume) {
            if (thisKlassName == "UserCostumeCollection") return IsSolisMasterIdExists(idStr, SolisMasterIdType::CostumeId) || ret;
            if (thisKlassName == "UserHairCollection") return IsSolisMasterIdExists(idStr, SolisMasterIdType::HairId) || ret;
        }
        if (Config::unlockAllLive && thisKlassName == "UserMusicCollection") {
            return IsSolisMasterIdExists(idStr, SolisMasterIdType::MusicId) || ret;
        }
        if (Config::unlockAllPhotoPose && thisKlassName == "UserPhotoPoseCollection") {
            return IsSolisMasterIdExists(idStr, SolisMasterIdType::PhotoPoseId) || ret;
        }
        return ret;
    }

    DEFINE_HOOK(void*, Solis_UserCostumeCollection_GetSortedCharacterCostumes, (void* self, Il2cppString* characterId, void* mtd)) {
        auto ret = Solis_UserCostumeCollection_GetSortedCharacterCostumes_Orig(self, characterId, mtd);
        if (!Config::unlockAllLiveCostume || !characterId) return ret;
        return AddSolisCostumesToUserCollection(ret, characterId->ToString());
    }

    DEFINE_HOOK(void*, Solis_UserCostumeCollection_GetOrderSortedCharacterCostumes, (void* self, Il2cppString* characterId, void* mtd)) {
        auto ret = Solis_UserCostumeCollection_GetOrderSortedCharacterCostumes_Orig(self, characterId, mtd);
        if (!Config::unlockAllLiveCostume || !characterId) return ret;
        return AddSolisCostumesToUserCollection(ret, characterId->ToString());
    }

    DEFINE_HOOK(void*, Solis_UserHairCollection_GetAllList, (void* self, void* comparison, void* mtd)) {
        auto ret = Solis_UserHairCollection_GetAllList_Orig(self, comparison, mtd);
        if (!Config::unlockAllLiveCostume) return ret;

        auto this_klass = Il2cppUtils::get_class_from_instance(self);
        if (!this_klass || std::string(this_klass->name) != "UserHairCollection") return ret;
        return AddSolisHairsToUserCollection(ret);
    }

    DEFINE_HOOK(bool, Solis_UserHairCollection_Exists, (void* self, Il2cppString* hairId, void* mtd)) {
        auto ret = Solis_UserHairCollection_Exists_Orig(self, hairId, mtd);
        if (!Config::unlockAllLiveCostume || !hairId) return ret;
        return IsSolisMasterIdExists(hairId->ToString(), SolisMasterIdType::HairId) || ret;
    }

    DEFINE_HOOK(void*, Solis_UserHairCollection_GetSortedCharacterHairs,
        (void* self, Il2cppString* characterId, void* costume, bool isBaseHairOnly, void* mtd)) {
        auto ret = Solis_UserHairCollection_GetSortedCharacterHairs_Orig(self, characterId, costume, isBaseHairOnly, mtd);
        if (!Config::unlockAllLiveCostume || !characterId) return ret;
        return AddSolisHairsToUserCollection(ret, characterId->ToString());
    }

    DEFINE_HOOK(void*, Solis_UserHairCollection_GetSortedCharacterOrnamentHairs,
        (void* self, Il2cppString* hairId, void* mtd)) {
        auto ret = Solis_UserHairCollection_GetSortedCharacterOrnamentHairs_Orig(self, hairId, mtd);
        if (!Config::unlockAllLiveCostume) return ret;

        static auto UserHairCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                     "UserHairCollection");
        static auto UserHairCollection_GetAllList_mtd = UserHairCollection_klass ?
                Il2cppUtils::il2cpp_class_get_method_from_name(UserHairCollection_klass->address, "GetAllList", 1) : nullptr;
        static auto UserHairCollection_GetAllList = UserHairCollection_GetAllList_mtd ?
                reinterpret_cast<void* (*)(void*, void*)>(UserHairCollection_GetAllList_mtd->methodPointer) : nullptr;
        if (!UserHairCollection_GetAllList) return ret;

        auto origList = UserHairCollection_GetAllList(self, nullptr);
        return AddSolisHairsToUserCollection(origList);
    }

    DEFINE_HOOK(bool, Solis_UserHairCollection_ExistsCharacterOrnamentHair,
        (void* self, Il2cppString* characterId, void* mtd)) {
        auto ret = Solis_UserHairCollection_ExistsCharacterOrnamentHair_Orig(self, characterId, mtd);
        if (!Config::unlockAllLiveCostume || !characterId) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_CostumeSpecifierExtensions_IsAppropriateCostume,
        (void* costumeSpecifier, void* costume, void* mtd)) {
        auto ret = Solis_CostumeSpecifierExtensions_IsAppropriateCostume_Orig(costumeSpecifier, costume, mtd);
        if (!Config::unlockAllLiveCostume || !costume) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_Hair_IsWearableCostume, (void* self, void* costume, void* mtd)) {
        auto ret = Solis_Hair_IsWearableCostume_Orig(self, costume, mtd);
        if (!Config::unlockAllLiveCostume || !self || !costume) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_ActivityCharacterSelectCommonModel_IsAppropriateCostume,
        (void* self, void* costume, void* mtd)) {
        auto ret = Solis_ActivityCharacterSelectCommonModel_IsAppropriateCostume_Orig(self, costume, mtd);
        if (!Config::unlockAllLiveCostume || !costume) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_RefreshLevelSelectScreenModel_IsAppropriateCostume,
        (void* self, void* refreshLevel, void* costume, void* mtd)) {
        auto ret = Solis_RefreshLevelSelectScreenModel_IsAppropriateCostume_Orig(self, refreshLevel, costume, mtd);
        if (!Config::unlockAllLiveCostume || !costume) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_CostumeChangeSheetModel_IsWearableCostume,
        (void* self, void* hair, void* mtd)) {
        auto ret = Solis_CostumeChangeSheetModel_IsWearableCostume_Orig(self, hair, mtd);
        if (!Config::unlockAllLiveCostume || !hair) return ret;
        return true;
    }

    DEFINE_HOOK(void*, Solis_UserMusicCollection_GetAllList, (void* self, void* comparison, void* mtd)) {
        auto ret = Solis_UserMusicCollection_GetAllList_Orig(self, comparison, mtd);
        if (!Config::unlockAllLive) return ret;

        auto this_klass = Il2cppUtils::get_class_from_instance(self);
        if (!this_klass || std::string(this_klass->name) != "UserMusicCollection") return ret;
        return AddSolisMusicsToUserCollection(ret);
    }

    DEFINE_HOOK(bool, Solis_UserMusicCollection_Exists, (void* self, Il2cppString* musicId, void* mtd)) {
        auto ret = Solis_UserMusicCollection_Exists_Orig(self, musicId, mtd);
        if (!Config::unlockAllLive || !musicId) return ret;
        return IsSolisMasterIdExists(musicId->ToString(), SolisMasterIdType::MusicId) || ret;
    }

    DEFINE_HOOK(bool, Solis_UserCharacterMusicCollection_Exists, (void* self, Il2cppString* characterId, Il2cppString* musicId, void* mtd)) {
        auto ret = Solis_UserCharacterMusicCollection_Exists_Orig(self, characterId, musicId, mtd);
        if (!Config::unlockAllLive || !musicId) return ret;
        return IsSolisMasterIdExists(musicId->ToString(), SolisMasterIdType::MusicId) || ret;
    }

    DEFINE_HOOK(void*, Solis_UserPhotoPoseCollection_GetUserPhotoPoses,
        (void* self, Il2cppString* characterId, bool checkReleased, void* mtd)) {
        auto ret = Solis_UserPhotoPoseCollection_GetUserPhotoPoses_Orig(self, characterId, checkReleased, mtd);
        if (!Config::unlockAllPhotoPose || !characterId) return ret;

        static auto get_PhotoPoseMotionMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                       "MasterManager", "get_PhotoPoseMotionMaster");
        static auto PhotoPoseMotionMaster_GetCharacterPoses = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                                                                                     "Solis.Common.Master",
                                                                                     "PhotoPoseMotionMaster",
                                                                                     "GetCharacterPoses",
                                                                                     {"System.String", "System.Boolean"});
        if (!get_PhotoPoseMotionMaster || !PhotoPoseMotionMaster_GetCharacterPoses) return ret;

        auto master = get_PhotoPoseMotionMaster->Invoke<void*>(nullptr);
        if (!master) return ret;

        auto allPoses = PhotoPoseMotionMaster_GetCharacterPoses->Invoke<void*>(master, characterId, false);
        return allPoses ? allPoses : ret;
    }

    DEFINE_HOOK(bool, Solis_PhotoPose_IsReleased, (void* self, void* mtd)) {
        auto ret = Solis_PhotoPose_IsReleased_Orig(self, mtd);
        if (!Config::unlockAllPhotoPose) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoMusic_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoMusic_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoPanoramaMusic_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoPanoramaMusic_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoContestQuestMusic_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoContestQuestMusic_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_SpecialPhotoQuestMusicInfo_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_SpecialPhotoQuestMusicInfo_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoActivity_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoActivity_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoContestActivity_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoContestActivity_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_SpecialPhotoActivityInfo_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_SpecialPhotoActivityInfo_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoStage_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoStage_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoContestQuestStage_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoContestQuestStage_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_SpecialPhotoQuestStage_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_SpecialPhotoQuestStage_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoContestSectionInfo_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoContestSectionInfo_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_PhotoExpression_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_PhotoExpression_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_SpecialPhotoShootingInfo_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_SpecialPhotoShootingInfo_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_ActivityPhotographyGridListItemModel_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_ActivityPhotographyGridListItemModel_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_ExpressionPhotographyGridListItemModel_get_IsUnlocked, (void* self, void* mtd)) {
        auto ret = Solis_ExpressionPhotographyGridListItemModel_get_IsUnlocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(bool, Solis_LivePhotographyMusicListItemModel_get_IsUnLocked, (void* self, void* mtd)) {
        auto ret = Solis_LivePhotographyMusicListItemModel_get_IsUnLocked_Orig(self, mtd);
        if (!Config::unlockAllLive) return ret;
        return true;
    }

    DEFINE_HOOK(void, Solis_LivePhotographyMusicListItemModel_set_IsUnLocked, (void* self, bool value, void* mtd)) {
        if (Config::unlockAllLive) value = true;
        return Solis_LivePhotographyMusicListItemModel_set_IsUnLocked_Orig(self, value, mtd);
    }

    bool needRestoreHides = false;

    DEFINE_HOOK(bool, VLDOF_IsActive, (void* self)) {
        if (Config::enableFreeCamera) return false;
        return VLDOF_IsActive_Orig(self);
    }

    DEFINE_HOOK(void, SolisQualityManager_set_TargetFrameRate, (void* self, float value)) {
        const auto configFps = Config::targetFrameRate;
        SolisQualityManager_set_TargetFrameRate_Orig(self, configFps == 0 ? value : (float)configFps);
    }

    DEFINE_HOOK(void, SolisQualityManager_ApplySetting, (void* self)) {
        if (Config::targetFrameRate != 0) {
            SolisQualityManager_set_TargetFrameRate_Orig(self, Config::targetFrameRate);
        }
        SolisQualityManager_ApplySetting_Orig(self);
    }

    DEFINE_HOOK(void, UIManager_UpdateRenderTarget, (UnityResolve::UnityType::Vector2 ratio, void* mtd)) {
        // const auto resolution = GetResolution();
        // Log::DebugFmt("UIManager_UpdateRenderTarget: %f, %f", ratio.x, ratio.y);
        return UIManager_UpdateRenderTarget_Orig(ratio, mtd);
    }

    DEFINE_HOOK(void, VLSRPCameraController_UpdateRenderTarget, (void* self, int width, int height, bool forceAlpha, void* method)) {
        // const auto resolution = GetResolution();
        // Log::DebugFmt("VLSRPCameraController_UpdateRenderTarget: %d, %d", width, height);
        return VLSRPCameraController_UpdateRenderTarget_Orig(self, width, height, forceAlpha, method);
    }

    DEFINE_HOOK(void*, VLUtility_GetLimitedResolution, (int32_t screenWidth, int32_t screenHeight,
            UnityResolve::UnityType::Vector2 aspectRatio, int32_t maxBufferPixel, float bufferScale, bool firstCall)) {

        //Log::DebugFmt("VLUtility_GetLimitedResolution: %d, %d, %f, %f", screenWidth, screenHeight, aspectRatio.x, aspectRatio.y);
        return VLUtility_GetLimitedResolution_Orig(screenWidth, screenHeight, aspectRatio, maxBufferPixel, bufferScale, firstCall);
    }

    bool InitBodyParts() {
        static auto isInit = false;
        if (isInit) return true;

        const auto Enum_GetValues = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Enum", "GetValues");
        const auto Enum_GetNames = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Enum", "GetNames");

        const auto HumanBodyBones_klass = Il2cppUtils::GetClass(
                "UnityEngine.AnimationModule.dll", "UnityEngine", "HumanBodyBones");

        const auto values = Enum_GetValues->Invoke<UnityResolve::UnityType::Array<int>*>(HumanBodyBones_klass->GetType())->ToVector();
        const auto names = Enum_GetNames->Invoke<UnityResolve::UnityType::Array<Il2cppString*>*>(HumanBodyBones_klass->GetType())->ToVector();
        if (values.size() != names.size()) {
            Log::ErrorFmt("InitBodyParts Error: values count: %ld, names count: %ld", values.size(), names.size());
            return false;
        }

        std::vector<std::string> namesVec{};
        namesVec.reserve(names.size());
        for (auto i :names) {
            namesVec.push_back(i->ToString());
        }
        IPCamera::bodyPartsEnum = Misc::CSEnum(namesVec, values);
        IPCamera::bodyPartsEnum.SetIndex(IPCamera::bodyPartsEnum.GetValueByName("Head"));
        isInit = true;
        return true;
    }

    void HideHead(UnityResolve::UnityType::GameObject* obj, const bool isFace) {
        static UnityResolve::UnityType::GameObject* lastFaceObj = nullptr;
        static UnityResolve::UnityType::GameObject* lastHairObj = nullptr;

#define lastHidedObj (isFace ? lastFaceObj : lastHairObj)

       static auto get_activeInHierarchy = reinterpret_cast<bool (*)(void*)>(
                Il2cppUtils::il2cpp_resolve_icall("UnityEngine.GameObject::get_activeInHierarchy()"));

        const auto isFirstPerson = IPCamera::GetCameraMode() == IPCamera::CameraMode::FIRST_PERSON;

        if (isFirstPerson && obj) {
            if (obj == lastHidedObj) return;
            if (lastHidedObj && IsNativeObjectAlive(lastHidedObj)/*&& get_activeInHierarchy(lastHidedObj)*/) {
                lastHidedObj->SetActive(true);
            }
            if (IsNativeObjectAlive(obj)) {
                obj->SetActive(false);
                lastHidedObj = obj;
            }
        }
        else {
            if (lastHidedObj && IsNativeObjectAlive(lastHidedObj)) {
                lastHidedObj->SetActive(true);
                lastHidedObj = nullptr;
            }
        }
    }

    DEFINE_HOOK(void, SolisActorController_LateUpdate, (void* self, void* mtd)) {
        static auto SolisActorController_klass = Il2cppUtils::GetClass("solis-submodule.Runtime.dll",
                                                                        "Solis.Common", "SolisActorController");
        static auto rootBody_field = SolisActorController_klass->Get<UnityResolve::Field>("_rootBody");
        static auto parentKlass = UnityResolve::Invoke<void*>("il2cpp_class_get_parent", SolisActorController_klass->address);

        if (!Config::enableFreeCamera || (IPCamera::GetCameraMode() == IPCamera::CameraMode::FREE)) {
            if (needRestoreHides) {
                needRestoreHides = false;
                HideHead(nullptr, false);
                HideHead(nullptr, true);
            }
            return SolisActorController_LateUpdate_Orig(self, mtd);
        }

        static auto GetHumanBodyBoneTransform_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(parentKlass, "GetHumanBodyBoneTransform", 1);
        static auto GetHumanBodyBoneTransform = reinterpret_cast<UnityResolve::UnityType::Transform* (*)(void*, int)>(
                GetHumanBodyBoneTransform_mtd->methodPointer
                );
        static auto get_index_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(SolisActorController_klass->address, "get_index", 0);
        static auto get_Index = get_index_mtd ? reinterpret_cast<int (*)(void*)>(
                get_index_mtd->methodPointer) : [](void*){return 0;};

        const auto currIndex = get_Index(self);
        if (currIndex == IPCamera::followCharaIndex) {
            static auto initPartsSuccess = InitBodyParts();
            static auto headBodyId = initPartsSuccess ? IPCamera::bodyPartsEnum.GetValueByName("Head") : 0xA;
            const auto isFirstPerson = IPCamera::GetCameraMode() == IPCamera::CameraMode::FIRST_PERSON;

            auto targetTrans = GetHumanBodyBoneTransform(self,
                                                         isFirstPerson ? headBodyId : IPCamera::bodyPartsEnum.GetCurrent().second);

            if (targetTrans) {
                cacheTrans = targetTrans;
                cacheRotation = cacheTrans->GetRotation();
                cachePosition = cacheTrans->GetPosition();
                cacheForward = cacheTrans->GetForward();
                cacheLookAt = cacheTrans->GetPosition() + cacheTrans->GetForward() * 3;

                auto rootBody = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(self, rootBody_field);
                auto rootModel = rootBody->GetParent();
                auto rootModelChildCount = rootModel->GetChildCount();
                for (int i = 0; i < rootModelChildCount; i++) {
                    auto rootChild = rootModel->GetChild(i);
                    const auto childName = rootChild->GetName();
                    if (childName == "Root_Face") {
                        // 기존의 VLSkinningRenderer를 찾는 루프를 제거하고 
                        // Root_Face 오브젝트 자체를 바로 HideHead에 넘깁니다.
                        HideHead(rootChild->GetGameObject(), true);
                        needRestoreHides = true;
                    }
                    else if (childName == "Root_Hair") {
                        HideHead(rootChild->GetGameObject(), false);
                        needRestoreHides = true;
                    }
                }
            }
            else {
                cacheTrans = nullptr;
            }

        }

        SolisActorController_LateUpdate_Orig(self, mtd);
    }

    DEFINE_HOOK(bool, PlatformInformation_get_IsAndroid, ()) {
        if (Config::loginAsIOS) {
            return false;
        }
        // Log::DebugFmt("PlatformInformation_get_IsAndroid: 0x%x", ret);
        return PlatformInformation_get_IsAndroid_Orig();
    }

    DEFINE_HOOK(bool, PlatformInformation_get_IsIOS, ()) {
        if (Config::loginAsIOS) {
            return true;
        }
        // Log::DebugFmt("PlatformInformation_get_IsIOS: 0x%x", ret);
        return PlatformInformation_get_IsIOS_Orig();
    }

    DEFINE_HOOK(Il2cppString*, ApiBase_GetPlatformString, (void* self, void* mtd)) {
        if (Config::loginAsIOS) {
            return Il2cppString::New("iOS");
        }
        // Log::DebugFmt("ApiBase_GetPlatformString: %s", ret->ToString().c_str());
        return ApiBase_GetPlatformString_Orig(self, mtd);
    }

    void ProcessApiBase(void* self) {
        static void* processedIOS = nullptr;

        if (Config::loginAsIOS) {
            if (self == processedIOS) return;

            static auto ApiBase_klass = Il2cppUtils::get_class_from_instance(self);
            static auto platform_field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", ApiBase_klass, "_platform");
             auto platform = Il2cppUtils::ClassGetFieldValue<Il2cppString*>(self, platform_field);
             Log::DebugFmt("ProcessApiBase platform: %s", platform ? platform->ToString().c_str() : "null");
             if (platform) {
                 const auto origPlatform = platform->ToString();
                 if (origPlatform != "iOS") {
                     Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("iOS"));
                     processedIOS = self;
                 }
             }
             else {
                 Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("iOS"));
                 processedIOS = self;
             }
        }
        else {
            if (processedIOS) {
                Log::DebugFmt("Restore API");
                static auto ApiBase_klass = Il2cppUtils::get_class_from_instance(self);
                static auto platform_field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", ApiBase_klass, "_platform");

                Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("Android"));
                processedIOS = nullptr;
            }
        }
    }

    DEFINE_HOOK(void, ApiBase_ctor, (void* self, void* mtd)) {
        ApiBase_ctor_Orig(self, mtd);
        ProcessApiBase(self);
    }

    DEFINE_HOOK(void*, ApiBase_get_Instance, (void* mtd)) {
        auto ret = ApiBase_get_Instance_Orig(mtd);
        if (ret) {
            ProcessApiBase(ret);
        }
        return ret;
    }

    void UpdateSolisSwingBreastBones(void* breastBones) {
        if (!Config::enableBreastParam || !breastBones) return;

        static auto ActorSwingBreastBone_klass = Il2cppUtils::GetClass("ActorSwing.Runtime.dll", "ActorSwing",
                                                                       "ActorSwingBreastBone");
        static auto LimitInfo_klass = Il2cppUtils::GetClass("ActorSwing.Runtime.dll", "ActorSwing",
                                                            "LimitInfo");
        if (!ActorSwingBreastBone_klass || !LimitInfo_klass) return;

        static auto damping_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("damping");
        static auto stiffness_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("stiffness");
        static auto spring_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("spring");
        static auto average_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("average");
        static auto useArmCorrection_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("useArmCorrection");
        static auto leftBreast_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("leftBreast");
        static auto rightBreast_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rightBreast");
        static auto leftBreastEnd_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("leftBreastEnd");
        static auto rightBreastEnd_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rightBreastEnd");
        static auto limitInfo_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("limitInfo");

        static auto limitInfo_useLimit_field = LimitInfo_klass->Get<UnityResolve::Field>("useLimit");
        static auto limitInfo_axisX_field = LimitInfo_klass->Get<UnityResolve::Field>("axisX");
        static auto limitInfo_axisY_field = LimitInfo_klass->Get<UnityResolve::Field>("axisY");
        static auto limitInfo_axisZ_field = LimitInfo_klass->Get<UnityResolve::Field>("axisZ");

        if (!damping_field || !stiffness_field || !spring_field || !average_field || !useArmCorrection_field ||
            !leftBreast_field || !rightBreast_field || !leftBreastEnd_field || !rightBreastEnd_field ||
            !limitInfo_field || !limitInfo_useLimit_field || !limitInfo_axisX_field ||
            !limitInfo_axisY_field || !limitInfo_axisZ_field) {
            return;
        }

        Il2cppUtils::Tools::CSListEditor<void*> listEditor(breastBones);
        for (auto bone : listEditor) {
            if (!bone) continue;

            if (Config::bUseScale) {
                auto leftBreast = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, leftBreast_field);
                auto rightBreast = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, rightBreast_field);
                auto leftBreastEnd = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, leftBreastEnd_field);
                auto rightBreastEnd = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, rightBreastEnd_field);

                const auto setScale = UnityResolve::UnityType::Vector3(Config::bScale, Config::bScale, Config::bScale);
                if (leftBreast) leftBreast->SetLocalScale(setScale);
                if (rightBreast) rightBreast->SetLocalScale(setScale);
                if (leftBreastEnd) leftBreastEnd->SetLocalScale(setScale);
                if (rightBreastEnd) rightBreastEnd->SetLocalScale(setScale);
            }

            auto limitInfo = Il2cppUtils::ClassGetFieldValue<void*>(bone, limitInfo_field);
            if (limitInfo) {
                if (!Config::bUseLimit) {
                    Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_useLimit_field, 0);
                }
                else {
                    Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_useLimit_field, 1);
                    auto axisX = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Vector2Int>(limitInfo, limitInfo_axisX_field);
                    auto axisY = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Vector2Int>(limitInfo, limitInfo_axisY_field);
                    auto axisZ = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Vector2Int>(limitInfo, limitInfo_axisZ_field);
                    axisX.m_X *= Config::bLimitXx;
                    axisX.m_Y *= Config::bLimitXy;
                    axisY.m_X *= Config::bLimitYx;
                    axisY.m_Y *= Config::bLimitYy;
                    axisZ.m_X *= Config::bLimitZx;
                    axisZ.m_Y *= Config::bLimitZy;
                    Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_axisX_field, axisX);
                    Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_axisY_field, axisY);
                    Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_axisZ_field, axisZ);
                }
            }

            Il2cppUtils::ClassSetFieldValue(bone, damping_field, Config::bDamping);
            Il2cppUtils::ClassSetFieldValue(bone, stiffness_field, Config::bStiffness);
            Il2cppUtils::ClassSetFieldValue(bone, spring_field, Config::bSpring);
            Il2cppUtils::ClassSetFieldValue(bone, average_field, Config::bAverage);
            Il2cppUtils::ClassSetFieldValue(bone, useArmCorrection_field, Config::bUseArmCorrection);
        }
    }

    DEFINE_HOOK(void*, SolisActorController_InitializeActorSwing_GetBreastBones,
        (void* self, void* modelParts, void* mtd)) {
        auto ret = SolisActorController_InitializeActorSwing_GetBreastBones_Orig(self, modelParts, mtd);
        UpdateSolisSwingBreastBones(ret);
        return ret;
    }

    void StartInjectFunctions() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

        UnityResolve::Init(xdl_open(hookInstaller->m_il2cppLibraryPath.c_str(), RTLD_NOW),
            UnityResolve::Mode::Il2Cpp, Config::lazyInit);

        ADD_HOOK(AssetBundle_LoadAssetAsync, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)"));
        ADD_HOOK(AssetBundleRequest_GetResult, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.AssetBundleRequest::GetResult()"));
        ADD_HOOK(Resources_Load, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.ResourcesAPIInternal::Load(System.String,System.Type)"));

        ADD_HOOK(I18nHelper_SetUpI18n, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
                                                                     "I18nHelper", "SetUpI18n"));
        ADD_HOOK(I18nHelper_SetValue, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
                                                                     "I18n", "SetValue"));

        //ADD_HOOK(UI_I18n_GetOrDefault, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
        //                                                             "I18n", "GetOrDefault"));

        ADD_HOOK(TextMeshProUGUI_Awake, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                      "TextMeshProUGUI", "Awake"));

        // TMP_FontAsset.Awake 훅: SourceSansPro-Regular (UABEA 교체본) 로드 시점 캡처
        ADD_HOOK(TMP_FontAsset_Awake, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                    "TMP_FontAsset", "Awake"));

        ADD_HOOK(TMP_Text_set_text, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                  "TMP_Text", "set_text"));
        auto addrSetText1 = Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                  "TMP_Text", "SetText",
                                                                  {"System.String"});
        ADD_HOOK(TMP_Text_SetText_1, addrSetText1);

        ADD_HOOK(TMP_Text_PopulateTextBackingArray, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                  "TMP_Text", "PopulateTextBackingArray",
                                                                  {"System.String", "System.Int32", "System.Int32"}));

        auto addrSetText2 = Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
            "TMP_Text", "SetText",
            { "System.String", "System.Boolean" });
        if (addrSetText2 && addrSetText2 != addrSetText1) {
            ADD_HOOK(TMP_Text_SetText_2, addrSetText2);
        }

        ADD_HOOK(TextField_set_value, Il2cppUtils::GetMethodPointer("UnityEngine.UIElementsModule.dll", "UnityEngine.UIElements",
                                                                  "TextField", "set_value"));

        // Legacy UnityEngine.UI.Text hook
        {
            auto uiTextPtr = Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI",
                                                           "Text", "set_text");
            if (uiTextPtr) {
                ADD_HOOK(UIText_set_text, uiTextPtr);
            }
            else {
                Log::InfoFmt("UIText_set_text: method not found, legacy UI.Text hook skipped.");
            }
        }

        ADD_HOOK(TMP_Text_SetCharArray, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
            "TMP_Text", "SetCharArray", {"System.Char[]", "System.Int32", "System.Int32"}));
        /* SQL 查询相关函数，不好用
        // 下面是 byte[] u8 string 转 std::string 的例子
        auto query = reinterpret_cast<UnityResolve::UnityType::Array<UnityResolve::UnityType::Byte>*>(mtd);
        auto data_ptr = reinterpret_cast<std::uint8_t*>(query->GetData());
        std::string qS(data_ptr, data_ptr + lastLength);

        ADD_HOOK(PreparedStatement_ExecuteQuery, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master.SQLite",
                                                                               "PreparedStatement", "ExecuteQuery", {"System.String"}));
        ADD_HOOK(PreparedStatement_ExecuteQuery_u8, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master.SQLite",
                                                                                  "PreparedStatement", "ExecuteQuery", {"*", "*"}));
        ADD_HOOK(PreparedStatement_FinalizeStatement, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master.SQLite",
                                                                                  "PreparedStatement", "FinalizeStatement"));
       */

        ADD_HOOK(MessageExtensions_MergeFrom, Il2cppUtils::GetMethodPointer("Google.Protobuf.dll", "Google.Protobuf",
                                                                            "MessageExtensions", "MergeFrom", {"Google.Protobuf.IMessage", "System.ReadOnlySpan<System.Byte>"}));

        ADD_HOOK(OctoCaching_GetResourceFileName, Il2cppUtils::GetMethodPointer("Octo.dll", "Octo.Caching",
                                                                     "OctoCaching", "GetResourceFileName"));

        ADD_HOOK(OctoResourceLoader_LoadFromCacheOrDownload,
                 Il2cppUtils::GetMethodPointer("Octo.dll", "Octo.Loader",
                                               "OctoResourceLoader", "LoadFromCacheOrDownload",
                                               {"System.String", "System.Action<System.String,Octo.LoadError>", "Octo.OnDownloadProgress"}));


        ADD_HOOK(Graphic_OnEnable,
                 Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI",
                                               "Graphic", "OnEnable", {}));
        ADD_HOOK(AssetBundle_LoadAsset,
                 Il2cppUtils::GetMethodPointer("UnityEngine.AssetBundleModule.dll", "UnityEngine",
                                               "AssetBundle", "LoadAsset", {"System.String"}));

        ADD_HOOK(OnDownloadProgress_Invoke,
                 Il2cppUtils::GetMethodPointer("Octo.dll", "Octo",
                                               "OnDownloadProgress", "Invoke"));

        
        auto loadLiveResultPtr = Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Live",
                                                               "LiveUtility", "LoadLiveResult",
                                                               {"Solis.Live.LiveStartTransitionData"});
        LiveUtility_LoadLiveResult_Call = reinterpret_cast<LiveUtility_LoadLiveResult_Type>(loadLiveResultPtr);
        ADD_HOOK(LiveUtility_LoadLiveScene_FullSkip,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Live",
                                               "LiveUtility", "LoadLiveScene",
                                               {"Solis.Live.LiveStartTransitionData", "System.Boolean"}));

        auto Solis_UserCostumeCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                       "UserCostumeCollection");
        if (Solis_UserCostumeCollection_klass) {
            auto FindBy_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserCostumeCollection_klass->address, "FindBy", 1);
            if (FindBy_mtd) {
                ADD_HOOK(Solis_UserCostumeCollection_FindBy, FindBy_mtd->methodPointer);
            }

            auto GetAllList_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserCostumeCollection_klass->address, "GetAllList", 1);
            if (GetAllList_mtd) {
                ADD_HOOK(Solis_UserCostumeCollection_GetAllList, GetAllList_mtd->methodPointer);
            }

            auto Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserCostumeCollection_klass->address, "Exists", 1);
            if (Exists_mtd) {
                ADD_HOOK(Solis_UserCostumeCollection_Exists, Exists_mtd->methodPointer);
            }

            auto GetSortedCharacterCostumes_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_UserCostumeCollection_klass->address, "GetSortedCharacterCostumes", 1);
            if (GetSortedCharacterCostumes_mtd) {
                ADD_HOOK(Solis_UserCostumeCollection_GetSortedCharacterCostumes, GetSortedCharacterCostumes_mtd->methodPointer);
            }

            auto GetOrderSortedCharacterCostumes_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_UserCostumeCollection_klass->address, "GetOrderSortedCharacterCostumes", 1);
            if (GetOrderSortedCharacterCostumes_mtd) {
                ADD_HOOK(Solis_UserCostumeCollection_GetOrderSortedCharacterCostumes, GetOrderSortedCharacterCostumes_mtd->methodPointer);
            }
        }

        auto Solis_UserHairCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                    "UserHairCollection");
        if (Solis_UserHairCollection_klass) {
            auto GetAllList_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserHairCollection_klass->address, "GetAllList", 1);
            if (GetAllList_mtd) {
                ADD_HOOK(Solis_UserHairCollection_GetAllList, GetAllList_mtd->methodPointer);
            }

            auto Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserHairCollection_klass->address, "Exists", 1);
            if (Exists_mtd) {
                ADD_HOOK(Solis_UserHairCollection_Exists, Exists_mtd->methodPointer);
            }

            auto GetSortedCharacterHairs_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_UserHairCollection_klass->address, "GetSortedCharacterHairs", 3);
            if (GetSortedCharacterHairs_mtd) {
                ADD_HOOK(Solis_UserHairCollection_GetSortedCharacterHairs, GetSortedCharacterHairs_mtd->methodPointer);
            }

            auto GetSortedCharacterOrnamentHairs_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_UserHairCollection_klass->address, "GetSortedCharacterOrnamentHairs", 1);
            if (GetSortedCharacterOrnamentHairs_mtd) {
                ADD_HOOK(Solis_UserHairCollection_GetSortedCharacterOrnamentHairs,
                         GetSortedCharacterOrnamentHairs_mtd->methodPointer);
            }

            auto ExistsCharacterOrnamentHair_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_UserHairCollection_klass->address, "ExistsCharacterOrnamentHair", 1);
            if (ExistsCharacterOrnamentHair_mtd) {
                ADD_HOOK(Solis_UserHairCollection_ExistsCharacterOrnamentHair,
                         ExistsCharacterOrnamentHair_mtd->methodPointer);
            }
        }

        auto Solis_UserPhotoPoseCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                         "UserPhotoPoseCollection");
        if (Solis_UserPhotoPoseCollection_klass) {
            auto GetUserPhotoPoses_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_UserPhotoPoseCollection_klass->address, "GetUserPhotoPoses", 2);
            if (GetUserPhotoPoses_mtd) {
                ADD_HOOK(Solis_UserPhotoPoseCollection_GetUserPhotoPoses, GetUserPhotoPoses_mtd->methodPointer);
            }
        }

        ADD_HOOK(Solis_CostumeSpecifierExtensions_IsAppropriateCostume,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Data",
                                               "CostumeSpecifierExtensions", "IsAppropriateCostume"));
        ADD_HOOK(Solis_Hair_IsWearableCostume,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "Hair", "IsWearableCostume"));
        ADD_HOOK(Solis_Costume_get_IsUserUnavailable,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "Costume", "get_IsUserUnavailable"));
        ADD_HOOK(Solis_Costume_get_IsDisableAdmin,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "Costume", "get_IsDisableAdmin"));
        ADD_HOOK(Solis_Hair_get_IsDisableAdmin,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "Hair", "get_IsDisableAdmin"));
        ADD_HOOK(Solis_PhotoPose_IsReleased,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "PhotoPose", "IsReleased"));
        ADD_HOOK(Solis_Costume_get_ImpossiblePhotoPoseTypes,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "Costume", "get_ImpossiblePhotoPoseTypes"));
        ADD_HOOK(Solis_Hair_get_ImpossiblePhotoPoseTypes,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                               "Hair", "get_ImpossiblePhotoPoseTypes"));
        ADD_HOOK(Solis_ActorCostume_get_SdHair,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Data",
                                               "ActorCostume", "get_SdHair"));
        ADD_HOOK(Solis_ActorCostume_GetCurrentIntersectedPoseTypes,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Data",
                                               "ActorCostume", "GetCurrentIntersectedPoseTypes"));
        ADD_HOOK(Solis_ActorCostume_GetExpressionIntersectedPoseTypes,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Data",
                                               "ActorCostume", "GetExpressionIntersectedPoseTypes"));
        ADD_HOOK(Solis_ActivityCharacterSelectCommonModel_IsAppropriateCostume,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "ActivityCharacterSelectCommonModel", "IsAppropriateCostume"));
        ADD_HOOK(Solis_RefreshLevelSelectScreenModel_IsAppropriateCostume,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "RefreshLevelSelectScreenModel", "IsAppropriateCostume"));
        ADD_HOOK(Solis_CostumeChangeSheetModel_IsWearableCostume,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "CostumeChangeSheetModel", "IsWearableCostume"));

        auto Solis_UserMusicCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                     "UserMusicCollection");
        if (Solis_UserMusicCollection_klass) {
            auto GetAllList_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserMusicCollection_klass->address, "GetAllList", 1);
            if (GetAllList_mtd) {
                ADD_HOOK(Solis_UserMusicCollection_GetAllList, GetAllList_mtd->methodPointer);
            }

            auto Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserMusicCollection_klass->address, "Exists", 1);
            if (Exists_mtd) {
                ADD_HOOK(Solis_UserMusicCollection_Exists, Exists_mtd->methodPointer);
            }
        }

        auto Solis_UserCharacterMusicCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                              "UserCharacterMusicCollection");
        if (Solis_UserCharacterMusicCollection_klass) {
            auto Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserCharacterMusicCollection_klass->address, "Exists", 2);
            if (Exists_mtd) {
                ADD_HOOK(Solis_UserCharacterMusicCollection_Exists, Exists_mtd->methodPointer);
            }
        }

        ADD_HOOK(Solis_PhotoMusic_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoMusic", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoPanoramaMusic_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoPanoramaMusic", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoContestQuestMusic_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoContestQuestMusic", "get_IsUnlocked"));
        ADD_HOOK(Solis_SpecialPhotoQuestMusicInfo_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "SpecialPhotoQuestMusicInfo", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoActivity_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoActivity", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoContestActivity_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoContestActivity", "get_IsUnlocked"));
        ADD_HOOK(Solis_SpecialPhotoActivityInfo_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "SpecialPhotoActivityInfo", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoStage_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoStage", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoContestQuestStage_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoContestQuestStage", "get_IsUnlocked"));
        ADD_HOOK(Solis_SpecialPhotoQuestStage_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "SpecialPhotoQuestStage", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoContestSectionInfo_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoContestSectionInfo", "get_IsUnlocked"));
        ADD_HOOK(Solis_PhotoExpression_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "PhotoExpression", "get_IsUnlocked"));
        ADD_HOOK(Solis_SpecialPhotoShootingInfo_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common.Proto.Api",
                                               "SpecialPhotoShootingInfo", "get_IsUnlocked"));
        ADD_HOOK(Solis_ActivityPhotographyGridListItemModel_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "ActivityPhotographyGridListItemModel", "get_IsUnlocked"));
        ADD_HOOK(Solis_ExpressionPhotographyGridListItemModel_get_IsUnlocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "ExpressionPhotographyGridListItemModel", "get_IsUnlocked"));
        ADD_HOOK(Solis_LivePhotographyMusicListItemModel_get_IsUnLocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "LivePhotographyMusicListItemModel", "get_IsUnLocked"));
        ADD_HOOK(Solis_LivePhotographyMusicListItemModel_set_IsUnLocked,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "LivePhotographyMusicListItemModel", "set_IsUnLocked"));

        auto Solis_ApiPhoto_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                          "Api/Photo");
        if (Solis_ApiPhoto_klass) {
            auto Photo_c_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                       "Api/Photo/<>c");
            if (Photo_c_klass) {
                auto CheckShootingAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Photo_c_klass->address, "<CheckShootingAsync>b__26_0", 4);
                if (CheckShootingAsyncCall_mtd) {
                    ADD_HOOK(Solis_Photo_CheckShootingAsync_Call, CheckShootingAsyncCall_mtd->methodPointer);
                }

                auto CheckSpecialShootingAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Photo_c_klass->address, "<CheckSpecialShootingAsync>b__42_0", 4);
                if (CheckSpecialShootingAsyncCall_mtd) {
                    ADD_HOOK(Solis_Photo_CheckSpecialShootingAsync_Call, CheckSpecialShootingAsyncCall_mtd->methodPointer);
                }

                auto CheckExpressionShootingAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Photo_c_klass->address, "<CheckExpressionShootingAsync>b__66_0", 4);
                if (CheckExpressionShootingAsyncCall_mtd) {
                    ADD_HOOK(Solis_Photo_CheckExpressionShootingAsync_Call,
                             CheckExpressionShootingAsyncCall_mtd->methodPointer);
                }

                auto CreateShootingsAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Photo_c_klass->address, "<CreateShootingsAsync>b__29_0", 4);
                if (CreateShootingsAsyncCall_mtd) {
                    ADD_HOOK(Solis_Photo_CreateShootingsAsync_Call, CreateShootingsAsyncCall_mtd->methodPointer);
                }

                auto CreateSpecialShootingsAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Photo_c_klass->address, "<CreateSpecialShootingsAsync>b__45_0", 4);
                if (CreateSpecialShootingsAsyncCall_mtd) {
                    ADD_HOOK(Solis_Photo_CreateSpecialShootingsAsync_Call,
                             CreateSpecialShootingsAsyncCall_mtd->methodPointer);
                }

                auto CreateExpressionShootingsAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Photo_c_klass->address, "<CreateExpressionShootingsAsync>b__69_0", 4);
                if (CreateExpressionShootingsAsyncCall_mtd) {
                    ADD_HOOK(Solis_Photo_CreateExpressionShootingsAsync_Call,
                             CreateExpressionShootingsAsyncCall_mtd->methodPointer);
                }
            }
        }

        auto Solis_ApiPhotoContest_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                                 "Api/PhotoContest");
        if (Solis_ApiPhotoContest_klass) {
            auto PhotoContest_c_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                              "Api/PhotoContest/<>c");
            if (PhotoContest_c_klass) {
                auto CheckShootingAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        PhotoContest_c_klass->address, "<CheckShootingAsync>b__15_0", 4);
                if (CheckShootingAsyncCall_mtd) {
                    ADD_HOOK(Solis_PhotoContest_CheckShootingAsync_Call, CheckShootingAsyncCall_mtd->methodPointer);
                }

                auto SubmitShootingAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        PhotoContest_c_klass->address, "<SubmitShootingAsync>b__18_0", 4);
                if (SubmitShootingAsyncCall_mtd) {
                    ADD_HOOK(Solis_PhotoContest_SubmitShootingAsync_Call, SubmitShootingAsyncCall_mtd->methodPointer);
                }
            }
        }

        auto Solis_ApiPhotoPanorama_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                                  "Api/PhotoPanorama");
        if (Solis_ApiPhotoPanorama_klass) {
            auto PhotoPanorama_c_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                               "Api/PhotoPanorama/<>c");
            if (PhotoPanorama_c_klass) {
                auto CheckShootingAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        PhotoPanorama_c_klass->address, "<CheckShootingAsync>b__5_0", 4);
                if (CheckShootingAsyncCall_mtd) {
                    ADD_HOOK(Solis_PhotoPanorama_CheckShootingAsync_Call, CheckShootingAsyncCall_mtd->methodPointer);
                }

                auto CreateShootingsAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        PhotoPanorama_c_klass->address, "<CreateShootingsAsync>b__8_0", 4);
                if (CreateShootingsAsyncCall_mtd) {
                    ADD_HOOK(Solis_PhotoPanorama_CreateShootingsAsync_Call,
                             CreateShootingsAsyncCall_mtd->methodPointer);
                }
            }
        }

        auto Solis_ApiCostume_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                            "Api/Costume");
        if (Solis_ApiCostume_klass) {
            auto Costume_c_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network",
                                                         "Api/Costume/<>c");
            if (Costume_c_klass) {
                auto SetCostumeAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Costume_c_klass->address, "<SetCostumeAsync>b__3_0", 4);
                if (SetCostumeAsyncCall_mtd) {
                    ADD_HOOK(Solis_Costume_SetCostumeAsync_Call, SetCostumeAsyncCall_mtd->methodPointer);
                }

                auto SetLiveCostumeAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Costume_c_klass->address, "<SetLiveCostumeAsync>b__6_0", 4);
                if (SetLiveCostumeAsyncCall_mtd) {
                    ADD_HOOK(Solis_Costume_SetLiveCostumeAsync_Call, SetLiveCostumeAsyncCall_mtd->methodPointer);
                }

                auto CheckBulkAsyncCall_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                        Costume_c_klass->address, "<CheckBulkAsync>b__12_0", 4);
                if (CheckBulkAsyncCall_mtd) {
                    ADD_HOOK(Solis_Costume_CheckBulkAsync_Call, CheckBulkAsyncCall_mtd->methodPointer);
                }
            }
        }

        // CharacterCostumeScreenPresenter.CheckBulkAsync returns UniTask by value.
        // Hooking this async state-machine entry directly is unstable on arm64/houdini;
        // keep the lower-level Costume:CheckBulk request sanitizer instead.

//        ADD_HOOK(UIManager_UpdateRenderTarget,
//                 Il2cppUtils::GetMethodPointer("ADV.Runtime.dll", "Solis.ADV",
//                                               "UIManager", "UpdateRenderTarget"));
        ADD_HOOK(VLSRPCameraController_UpdateRenderTarget,
                 Il2cppUtils::GetMethodPointer("vl-unity.Runtime.dll", "VL.Rendering",
                                               "VLSRPCameraController", "UpdateRenderTarget",
                                               {"*", "*", "*"}));

        ADD_HOOK(VLUtility_GetLimitedResolution,
                 Il2cppUtils::GetMethodPointer("vl-unity.Runtime.dll", "VL",
                                               "VLUtility", "GetLimitedResolution",
                                               {"*", "*", "*", "*", "*", "*"}));

        ADD_HOOK(SolisActorController_LateUpdate,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisActorController", "LateUpdate"));

        auto SolisActorController_c_klass = Il2cppUtils::GetClass("solis-submodule.Runtime.dll", "Solis.Common",
                                                                  "SolisActorController/<>c");
        if (SolisActorController_c_klass) {
            auto GetBreastBones_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    SolisActorController_c_klass->address, "<InitializeActorSwing>b__64_3", 1);
            if (GetBreastBones_mtd) {
                ADD_HOOK(SolisActorController_InitializeActorSwing_GetBreastBones,
                         GetBreastBones_mtd->methodPointer);
            }
        }

        ADD_HOOK(PlatformInformation_get_IsAndroid, Il2cppUtils::GetMethodPointer("Firebase.Platform.dll", "Firebase.Platform",
                                                                         "PlatformInformation", "get_IsAndroid"));
        ADD_HOOK(PlatformInformation_get_IsIOS, Il2cppUtils::GetMethodPointer("Firebase.Platform.dll", "Firebase.Platform",
                                                                                  "PlatformInformation", "get_IsIOS"));

        auto api_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network", "Api");
        if (api_klass) {
            // Qua.Network.ApiBase
            auto api_parent = UnityResolve::Invoke<Il2cppUtils::Il2CppClassHead*>("il2cpp_class_get_parent", api_klass->address);
            if (api_parent) {
                // Log::DebugFmt("api_parent at %p, name: %s::%s", api_parent, api_parent->namespaze, api_parent->name);
                ADD_HOOK(ApiBase_GetPlatformString, Il2cppUtils::il2cpp_class_get_method_pointer_from_name(api_parent, "GetPlatformString", 0));
                ADD_HOOK(ApiBase_ctor, Il2cppUtils::il2cpp_class_get_method_pointer_from_name(api_parent, ".ctor", 0));
                ADD_HOOK(ApiBase_get_Instance, Il2cppUtils::il2cpp_class_get_method_pointer_from_name(api_parent, "get_Instance", 0));
            }
        }

        ADD_HOOK(SolisQualityManager_set_TargetFrameRate,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "set_TargetFrameRate"));
        ADD_HOOK(SolisQualityManager_ApplySetting,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "ApplySetting"));

        ADD_HOOK(Internal_LogException, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_LogException(System.Exception,UnityEngine.Object)"));
        ADD_HOOK(Internal_Log, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_Log(UnityEngine.LogType,UnityEngine.LogOption,System.String,UnityEngine.Object)"));

        // 双端
        ADD_HOOK(InternalSetOrientationAsync,
            Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                "ScreenOrientationControllerBase", "InternalSetOrientationAsync"));

        ADD_HOOK(Unity_set_position_Injected, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)"));
        ADD_HOOK(Unity_set_rotation_Injected, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)"));
        ADD_HOOK(Unity_get_fieldOfView, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                      "Camera", "get_fieldOfView"));
        ADD_HOOK(Unity_set_fieldOfView, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                      "Camera", "set_fieldOfView"));
        ADD_HOOK(Unity_set_targetFrameRate, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.Application::set_targetFrameRate(System.Int32)"));
        ADD_HOOK(EndCameraRendering, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                                                                     "RenderPipeline", "EndCameraRendering"));

    }
    // 77 2640 5000

    DEFINE_HOOK(int, il2cpp_init, (const char* domain_name)) {
        const auto ret = il2cpp_init_Orig(domain_name);
        // InjectFunctions();

        Log::Info("Waiting for config...");

        while (!Config::isConfigInit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!Config::enabled) {
            Log::Info("Plugin not enabled");
            return ret;
        }

        Log::Info("Start init plugin...");

        if (Config::lazyInit) {
            UnityResolveProgress::startInit = true;
            UnityResolveProgress::assembliesProgress.total = 2;
            UnityResolveProgress::assembliesProgress.current = 1;
            UnityResolveProgress::classProgress.total = 29;
            UnityResolveProgress::classProgress.current = 0;
        }

        StartInjectFunctions();
        IPCamera::initCameraSettings();

        if (Config::lazyInit) {
            UnityResolveProgress::assembliesProgress.current = 2;
            UnityResolveProgress::classProgress.total = 1;
            UnityResolveProgress::classProgress.current = 0;
        }

        Local::LoadData();
        MasterLocal::LoadData();

        UnityResolveProgress::startInit = false;

        Log::Info("Plugin init finished.");
        return ret;
    }
}


namespace HoshimiLocal::Hook {
    void Install() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

        Log::Info("Installing hook");

        ADD_HOOK(HookMain::il2cpp_init,
            Plugin::GetInstance().GetHookInstaller()->LookupSymbol("il2cpp_init"));

        Log::Info("Hook installed");
    }
}
