#include "shadowhook.h"
#include "xdl.h"
#include <android/log.h>
#include "Hook.h"
#include "PrivateServer.hpp"
#include "Plugin.h"
#include "Log.h"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "Il2cppUtils.hpp"
#include "Local.h"
#include "MasterLocal.h"
#include "RenderResolution.hpp"
#include "GraphicsQuality.hpp"
#include "BreastMotion.hpp"
#include "PhotoRequests.hpp"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include "camera/camera.hpp"
#include "config/Config.hpp"
// #include <jni.h>
#include <thread>
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include <chrono>
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
    std::atomic<bool> clearUserDataRequested{false};
    std::atomic<int> clearUserDataResult{0};

    bool ClearPrivateServerAccount() {
        const auto klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common", "SolisInitializer");
        if (!klass) return false;
        const auto method = Il2cppUtils::il2cpp_class_get_method_from_name(klass->address, "ClearUserData", 1);
        if (!method || !method->methodPointer) return false;
        void* args[1] = {nullptr};
        void* exception = nullptr;
        UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", method, nullptr, args, &exception);
        return exception == nullptr;
    }
    using Il2cppString = UnityResolve::UnityType::String;

    DEFINE_HOOK(void, Solis_UserGetResponse_InternalMergeFrom, (void* self, void* input, void* mtd)) {
        Solis_UserGetResponse_InternalMergeFrom_Orig(self, input, mtd);
        PhotoRequests::CaptureInitial(self);
    }

    DEFINE_HOOK(void, Solis_UpdatedData_InternalMergeFrom, (void* self, void* input, void* mtd)) {
        Solis_UpdatedData_InternalMergeFrom_Orig(self, input, mtd);
        PhotoRequests::CaptureUpdate(self);
    }

    DEFINE_HOOK(void, Solis_PhotoListSpecialShootingResponse_InternalMergeFrom, (void* self, void* input, void* mtd)) {
        Solis_PhotoListSpecialShootingResponse_InternalMergeFrom_Orig(self, input, mtd);
        PhotoRequests::CaptureAvailability(self);
    }

    DEFINE_HOOK(void, Solis_PromotionProgress_InternalMergeFrom, (void* self, void* input, void* mtd)) {
        Solis_PromotionProgress_InternalMergeFrom_Orig(self, input, mtd);
        PhotoRequests::ApplyActivityVisualOutfits(self);
    }
    DEFINE_HOOK(void, Solis_RefreshProgress_InternalMergeFrom, (void* self, void* input, void* mtd)) {
        Solis_RefreshProgress_InternalMergeFrom_Orig(self, input, mtd);
        PhotoRequests::ApplyActivityVisualOutfits(self);
    }
    DEFINE_HOOK(void, Solis_FanEventProgress_InternalMergeFrom, (void* self, void* input, void* mtd)) {
        Solis_FanEventProgress_InternalMergeFrom_Orig(self, input, mtd);
        PhotoRequests::ApplyActivityVisualOutfits(self);
    }

    void LogImageResourceDebug(const char* source, void* component, const std::string& assetName, bool replaced) {
        if (!Config::debugImageResourceLog || assetName.empty()) return;
        Log::DebugFmt("ResourceDebug[%s]: asset=%s replaced=%d component=%p",
                      source, assetName.c_str(), replaced ? 1 : 0, component);
    }

    struct ImageDebugRect {
        float x;
        float y;
        float width;
        float height;
    };

    struct ImageDebugVector2 {
        float x;
        float y;
    };

    struct ImageDebugVector4 {
        float x;
        float y;
        float z;
        float w;
    };

    struct ImageColor32 {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    void LogSpriteReplacementDebug(const char* source, void* component, const std::string& assetName,
                                   void* originalSprite, void* replacementSprite) {
        if (!Config::debugImageResourceLog || !originalSprite || !replacementSprite) return;

        using GetRectInjected = void(*)(void*, ImageDebugRect*, void*);
        using GetVector2Injected = void(*)(void*, ImageDebugVector2*, void*);
        using GetVector4Injected = void(*)(void*, ImageDebugVector4*, void*);

        static auto getRect = reinterpret_cast<GetRectInjected>(Il2cppUtils::GetMethodPointer(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_rect_Injected", {"UnityEngine.Rect&"}));
        static auto getPivot = reinterpret_cast<GetVector2Injected>(Il2cppUtils::GetMethodPointer(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_pivot_Injected", {"UnityEngine.Vector2&"}));
        static auto getBorder = reinterpret_cast<GetVector4Injected>(Il2cppUtils::GetMethodPointer(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_border_Injected", {"UnityEngine.Vector4&"}));
        static auto getPixelsPerUnit = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_pixelsPerUnit");
        static auto getTexture = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_texture");
        static auto getTextureRect = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_textureRect");
        static auto getWidth = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Texture", "get_width");
        static auto getHeight = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Texture", "get_height");
        static auto getRectTransform = Il2cppUtils::GetMethod(
                "UnityEngine.UI.dll", "UnityEngine.UI", "Graphic", "get_rectTransform");
        static auto getUiRect = reinterpret_cast<GetRectInjected>(Il2cppUtils::GetMethodPointer(
                "UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform", "get_rect_Injected", {"UnityEngine.Rect&"}));

        ImageDebugRect originalRect{};
        ImageDebugRect replacementRect{};
        ImageDebugRect originalTextureRect{};
        ImageDebugRect replacementTextureRect{};
        ImageDebugRect uiRect{};
        ImageDebugVector2 originalPivot{};
        ImageDebugVector2 replacementPivot{};
        ImageDebugVector4 originalBorder{};
        ImageDebugVector4 replacementBorder{};

        if (getRect) {
            getRect(originalSprite, &originalRect, nullptr);
            getRect(replacementSprite, &replacementRect, nullptr);
        }
        if (getPivot) {
            getPivot(originalSprite, &originalPivot, nullptr);
            getPivot(replacementSprite, &replacementPivot, nullptr);
        }
        if (getBorder) {
            getBorder(originalSprite, &originalBorder, nullptr);
            getBorder(replacementSprite, &replacementBorder, nullptr);
        }

        float originalPpu = getPixelsPerUnit ? getPixelsPerUnit->Invoke<float>(originalSprite) : 0.0f;
        float replacementPpu = getPixelsPerUnit ? getPixelsPerUnit->Invoke<float>(replacementSprite) : 0.0f;
        if (getTextureRect) {
            originalTextureRect = getTextureRect->Invoke<ImageDebugRect>(originalSprite);
            replacementTextureRect = getTextureRect->Invoke<ImageDebugRect>(replacementSprite);
        }
        void* originalTexture = getTexture ? getTexture->Invoke<void*>(originalSprite) : nullptr;
        void* replacementTexture = getTexture ? getTexture->Invoke<void*>(replacementSprite) : nullptr;
        int originalTextureWidth = originalTexture && getWidth ? getWidth->Invoke<int>(originalTexture) : 0;
        int originalTextureHeight = originalTexture && getHeight ? getHeight->Invoke<int>(originalTexture) : 0;
        int replacementTextureWidth = replacementTexture && getWidth ? getWidth->Invoke<int>(replacementTexture) : 0;
        int replacementTextureHeight = replacementTexture && getHeight ? getHeight->Invoke<int>(replacementTexture) : 0;

        if (component && getRectTransform && getUiRect) {
            auto rectTransform = getRectTransform->Invoke<void*>(component);
            if (rectTransform) getUiRect(rectTransform, &uiRect, nullptr);
        }

        Log::DebugFmt(
                "SpriteDebug[%s]: asset=%s originalRect=%.1fx%.1f replacementRect=%.1fx%.1f "
                "originalTextureRect=%.1fx%.1f replacementTextureRect=%.1fx%.1f "
                "originalTexture=%dx%d replacementTexture=%dx%d originalPPU=%.2f replacementPPU=%.2f "
                "originalPivot=(%.1f,%.1f) replacementPivot=(%.1f,%.1f) "
                "originalBorder=(%.1f,%.1f,%.1f,%.1f) replacementBorder=(%.1f,%.1f,%.1f,%.1f) uiRect=%.1fx%.1f",
                source, assetName.c_str(), originalRect.width, originalRect.height,
                replacementRect.width, replacementRect.height,
                originalTextureRect.width, originalTextureRect.height,
                replacementTextureRect.width, replacementTextureRect.height,
                originalTextureWidth, originalTextureHeight,
                replacementTextureWidth, replacementTextureHeight, originalPpu, replacementPpu,
                originalPivot.x, originalPivot.y, replacementPivot.x, replacementPivot.y,
                originalBorder.x, originalBorder.y, originalBorder.z, originalBorder.w,
                replacementBorder.x, replacementBorder.y, replacementBorder.z, replacementBorder.w,
                uiRect.width, uiRect.height);
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

    struct SolisUniTask {
        void* source;
        int16_t token;
        int16_t padding0;
        int32_t padding1;
    };

    struct SolisCancellationToken {
        void* source;
    };

    static_assert(sizeof(SolisUniTask) == 16);
    static_assert(sizeof(SolisCancellationToken) == sizeof(void*));

    DEFINE_HOOK(SolisUniTask, InternalSetOrientationAsync,
                (void* self, int type, SolisCancellationToken cancellationToken, void* method)) {
        switch (Config::gameOrientation) {
        case 1: type = 0x2; break;  // FixedPortrait
        case 2: type = 0x3; break;  // FixedLandscape
        default: break;
        }
        return InternalSetOrientationAsync_Orig(self, type, cancellationToken, method);
    }

    void UpdatePhoneSubtitleOverlay();

    DEFINE_HOOK(void, EndCameraRendering, (void* ctx, void* camera, void* method)) {
        EndCameraRendering_Orig(ctx, camera, method);
        if (clearUserDataRequested.exchange(false)) {
            clearUserDataResult.store(ClearPrivateServerAccount() ? 1 : -1);
        }
        UpdatePhoneSubtitleOverlay();

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
    std::chrono::steady_clock::time_point runtimeKoreanFontNextLoadAttempt{};
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
            // Preserve managed string references and copy only numeric FaceInfo metrics.
            constexpr int metricOffset = 0x18;
            constexpr int metricSize = 0x60 - metricOffset;
            auto* src = reinterpret_cast<char*>(solisFontAsset) + faceInfoOffset + metricOffset;
            auto* dst = reinterpret_cast<char*>(sourceSansProAsset) + faceInfoOffset + metricOffset;
            std::memcpy(dst, src, metricSize);
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

            const auto now = std::chrono::steady_clock::now();
            if (now < runtimeKoreanFontNextLoadAttempt) return nullptr;
            runtimeKoreanFontNextLoadAttempt = now + std::chrono::seconds(5);

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
                runtimeKoreanFontNextLoadAttempt = {};
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

        auto get_special_batchim = [&text](size_t endPos) -> int {
            if (endPos == 0) return -1;

            const char lastByte = text[endPos - 1];
            if (lastByte >= '0' && lastByte <= '9') {
                static constexpr int digitBatchim[] = {
                        21, 8, 0, 16, 0, 0, 1, 8, 8, 0
                };
                return digitBatchim[lastByte - '0'];
            }

            size_t wordStart = endPos;
            while (wordStart > 0) {
                const char current = text[wordStart - 1];
                const bool isAsciiLetter = (current >= 'a' && current <= 'z') ||
                                           (current >= 'A' && current <= 'Z');
                if (!isAsciiLetter) break;
                wordStart--;
            }
            if (wordStart == endPos) return -1;

            std::string word = text.substr(wordStart, endPos - wordStart);
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char character) {
                if (character >= 'A' && character <= 'Z') return static_cast<char>(character + ('a' - 'A'));
                return static_cast<char>(character);
            });

            if (word == "fran") return 21; // 프랑
            if (word == "miho" || word == "kana") return 0; // 미호, 카나
            return -1;
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

            int batchim = get_special_batchim(pos);
            if (batchim < 0) batchim = get_batchim(lastChar);
            bool hasBatchim = (pos == 0) ? true : (batchim > 0);

            if (tag == "[은/는]") replaceWith = hasBatchim ? "\xEC\x9D\x80" : "\xEB\x8A\x94"; // 은 : 는
            else if (tag == "[이/가]") replaceWith = hasBatchim ? "\xEC\x9D\xB4" : "\xEA\xB0\x80"; // 이 : 가
            else if (tag == "[이/랑]" || tag == "[이랑/랑]") replaceWith = hasBatchim ? "\xEC\x9D\xB4\xEB\x9E\x91" : "\xEB\x9E\x91"; // 이랑 : 랑
            else if (tag == "[이/라]" || tag == "[이라/라]") replaceWith = hasBatchim ? "\xEC\x9D\xB4\xEB\x9D\xBC" : "\xEB\x9D\xBC"; // 이라 : 라
            else if (tag == "[이/다]" || tag == "[이다/다]") replaceWith = hasBatchim ? "\xEC\x9D\xB4\xEB\x8B\xA4" : "\xEB\x8B\xA4"; // 이다 : 다
            else if (tag == "[와/과]" || tag == "[과/와]") replaceWith = hasBatchim ? "\xEA\xB3\xBC" : "\xEC\x99\x80"; // 과 : 와
            else if (tag == "[을/를]") replaceWith = hasBatchim ? "\xEC\x9D\x84" : "\xEB\xA5\xBC"; // 을 : 를
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
        
        // 쉼표가 인게임에서 잘 보이는 하단작은따옴표로 치환해서 보여주게
        // 단, 텍스트가 숫자로만 이루어져 있는 경우는 제외
        bool isOnlyNumbers = true;
        for (char c : text) {
            if (!(c >= '0' && c <= '9') && 
                c != ',' && c != '.' && c != ' ' && c != '+' && c != '-' && c != '%' && c != 'x' && c != 'X' && c != '/') {
                isOnlyNumbers = false;
                break;
            }
        }

        if (!isOnlyNumbers) {
            while ((pos = text.find(",", pos)) != std::string::npos) {
                text.replace(pos, 1, "\xE2\x80\x9A");
                pos += 3;
            }
        }
        pos = 0;

        // 스토리창 범인: U+2E3A (Two-Em Dash, \xE2\xB8\xBA) 를 Em Dash(—) 하나로 치환
        while ((pos = text.find("\xE2\xB8\xBA", pos)) != std::string::npos) {
            text.replace(pos, 3, "\xE2\x80\x94");
            pos += 3;
        }

        // 사용자 닉네임을 먼저 치환해 받침 기준으로 조사를 선택
        text = Config::ReplaceDisplayUserName(std::move(text));

        // 한국어 조사 처리 추가
        text = ResolveJosa(text);

        return text;
    }

    DEFINE_HOOK(Il2cppString*, ADVEnginePresenterBase_get_UserName, (void* self, void* mtd)) {
        auto originalName = ADVEnginePresenterBase_get_UserName_Orig(self, mtd);
        if (Config::displayUserName.empty()) return originalName;
        return Il2cppString::New(Config::displayUserName);
    }

    DEFINE_HOOK(Il2cppString*, MessageDetail_GetReplacedMessage,
                (void* self, Il2cppString* userName, void* mtd)) {
        if (!Config::displayUserName.empty()) {
            userName = Il2cppString::New(Config::displayUserName);
        }
        return MessageDetail_GetReplacedMessage_Orig(self, userName, mtd);
    }

    DEFINE_HOOK(Il2cppString*, MessageDetail_GetNotificationText,
                (void* self, Il2cppString* userName, void* mtd)) {
        if (!Config::displayUserName.empty()) {
            userName = Il2cppString::New(Config::displayUserName);
        }
        return MessageDetail_GetNotificationText_Orig(self, userName, mtd);
    }

    std::unordered_set<void*> updatedFontPtrs{};
    std::unordered_set<void*> forcedTextPtrs{};
    std::unordered_set<void*> runtimeFallbackOriginalFontPtrs{};

    void TrackObservedFontAsset(void* fontAsset) {
        if (!fontAsset) return;
        if (std::find(observedFontAssets.begin(), observedFontAssets.end(), fontAsset) != observedFontAssets.end()) return;
        if (observedFontAssets.size() > 512) observedFontAssets.erase(observedFontAssets.begin());
        observedFontAssets.emplace_back(fontAsset);
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


    std::string currentPhoneSubtitleText{};
    void* phoneSubtitleTimerOwner = nullptr;
    void ClearPhoneSubtitle();

    std::string GetUnityTransformPath(void* transform, int maxDepth = 12) {
        if (!transform) return "<no transform>";

        static auto get_parent = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "get_parent");
        std::vector<std::string> names;
        void* current = transform;
        for (int depth = 0; current && IsNativeObjectAlive(current) && depth < maxDepth; ++depth) {
            names.emplace_back(GetUnityObjectName(current));
            if (!get_parent) break;
            current = get_parent->Invoke<void*>(current);
        }

        std::string path;
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            if (!path.empty()) path += "/";
            path += *it;
        }
        return path.empty() ? "<empty path>" : path;
    }

    std::string GetUnityComponentPath(void* component) {
        if (!component) return "<null component>";
        static auto get_transform = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Component", "get_transform");
        if (!get_transform) return GetUnityObjectName(component);
        auto transform = get_transform->Invoke<void*>(component);
        return GetUnityTransformPath(transform) + " [component=" + GetUnityObjectName(component) + "]";
    }

    void ApplyPhoneSubtitleTimerLayout(void* textComponent) {
        if (!textComponent || !IsNativeObjectAlive(textComponent)) return;
        static std::unordered_set<void*> styledTimers{};
        if (styledTimers.contains(textComponent)) return;

        static auto set_alignment = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_alignment", {"TMPro.TextAlignmentOptions"});
        static auto set_richText = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_richText", {"System.Boolean"});
        if (set_alignment) {
            // Bottom Center keeps the original MM:SS line pinned while subtitle lines grow upward.
            set_alignment->Invoke<void>(textComponent, 1026);
        }
        if (set_richText) set_richText->Invoke<void>(textComponent, true);
        styledTimers.emplace(textComponent);
    }
    bool IsPhoneCallTimerText(const std::string& text) {
        return text.length() == 5 &&
               std::isdigit(static_cast<unsigned char>(text[0])) &&
               std::isdigit(static_cast<unsigned char>(text[1])) &&
               text[2] == ':' &&
               std::isdigit(static_cast<unsigned char>(text[3])) &&
               std::isdigit(static_cast<unsigned char>(text[4]));
    }
    bool TryMakePhoneSubtitleTimerText(void* self, const std::string& origText, std::string* ret) {
        if (!Config::usePhoneSubtitles) return false;
        if (!ret || currentPhoneSubtitleText.empty()) return false;
        if (!IsPhoneCallTimerText(origText)) return false;

        const auto componentPath = GetUnityComponentPath(self);
        const bool isPhoneTalkTime = componentPath.find("Singleton/TelephoneManager/Canvas/Anchor/TalkTime") != std::string::npos;
        if (isPhoneTalkTime) {
            phoneSubtitleTimerOwner = self;
            ApplyPhoneSubtitleTimerLayout(self);
        }
        if (Config::debugAudioLog) {
            Log::DebugFmt("ResourceDebug[PhoneSubtitle.CandidateTimerText]: object=%p path=%s text=%s subtitle=%s match=%d",
                          self,
                          componentPath.c_str(),
                          origText.c_str(),
                          currentPhoneSubtitleText.c_str(),
                          isPhoneTalkTime ? 1 : 0);
        }
        if (!isPhoneTalkTime) return false;

        const auto localizedSubtitle = FixLigature(currentPhoneSubtitleText);
        const bool singleLineSubtitle = localizedSubtitle.find('\n') == std::string::npos;
        *ret = singleLineSubtitle
            ? "<size=85%><voffset=0.45em>" + localizedSubtitle + "</voffset></size>\n" + origText
            : "<size=85%>" + localizedSubtitle + "</size>\n" + origText;
        return true;
    }
    std::string MakeLocalizedText(const std::string& origText, const std::string& transText, bool hasTrans) {
        auto finalStr = FixLigature(hasTrans ? transText : origText);
        Local::DumpRemainingJapaneseText(finalStr);
        return finalStr;
    }

    bool HasTextLayoutMarker(const std::string& text) {
        return text.starts_with("[center]");
    }

    void ApplyCenterTextLayout(void* textComponent) {
        if (!textComponent) return;
        static auto set_alignment = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_alignment", {"TMPro.TextAlignmentOptions"});
        static auto set_richText = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_richText", {"System.Boolean"});
        if (set_richText) set_richText->Invoke<void>(textComponent, true);
        if (set_alignment) set_alignment->Invoke<void>(textComponent, 514);
    }

    bool ApplyTextLayoutMarkers(void* textComponent, std::string& text) {
        if (!textComponent || !HasTextLayoutMarker(text)) return false;
        text.erase(0, 8);
        ApplyCenterTextLayout(textComponent);
        return true;
    }

    DEFINE_HOOK(void, TMP_Text_PopulateTextBackingArray, (void* self, UnityResolve::UnityType::String* text, int start, int length)) {
        if (!text) {
            return TMP_Text_PopulateTextBackingArray_Orig(self, text, start, length);
        }

        static auto Substring = Il2cppUtils::GetMethod("mscorlib.dll", "System", "String", "Substring",
                                                       {"System.Int32", "System.Int32"});

        const std::string origText = Substring->Invoke<Il2cppString*>(text, start, length)->ToString();
        std::string phoneSubtitleText;
        if (TryMakePhoneSubtitleTimerText(self, origText, &phoneSubtitleText)) {
            const auto newText = UnityResolve::UnityType::String::New(phoneSubtitleText);
            UpdateFont(self);
            TMP_Text_PopulateTextBackingArray_Orig(self, newText, 0, newText->length);
            UpdateFont(self);
            return;
        }
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText || HasTextLayoutMarker(finalStr)) {
            ApplyTextLayoutMarkers(self, finalStr);
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
        std::string phoneSubtitleText;
        if (TryMakePhoneSubtitleTimerText(self, origText, &phoneSubtitleText)) {
            const auto newText = UnityResolve::UnityType::String::New(phoneSubtitleText);
            UpdateFont(self);
            TMP_Text_set_text_Orig(self, newText, mtd);
            UpdateFont(self);
            return;
        }
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText || HasTextLayoutMarker(finalStr)) {
            ApplyTextLayoutMarkers(self, finalStr);
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
        std::string phoneSubtitleText;
        if (TryMakePhoneSubtitleTimerText(self, origText, &phoneSubtitleText)) {
            const auto newText = UnityResolve::UnityType::String::New(phoneSubtitleText);
            UpdateFont(self);
            TMP_Text_SetText_1_Orig(self, newText, mtd);
            UpdateFont(self);
            return;
        }
        std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

        if (hasTrans || finalStr != origText || HasTextLayoutMarker(finalStr)) {
            ApplyTextLayoutMarkers(self, finalStr);
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
        std::string phoneSubtitleText;
        if (TryMakePhoneSubtitleTimerText(self, origText, &phoneSubtitleText)) {
            const auto newText = UnityResolve::UnityType::String::New(phoneSubtitleText);
            UpdateFont(self);
            TMP_Text_SetText_2_Orig(self, newText, syncTextInputBox, mtd);
            UpdateFont(self);
            return;
        }
		std::string transText;
        bool hasTrans = Local::GetGenericText(origText, &transText);
        std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

		if (hasTrans || finalStr != origText || HasTextLayoutMarker(finalStr)) {
            ApplyTextLayoutMarkers(self, finalStr);
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
        static auto backingStringDirtyField = TMP_Text_klass->Get<UnityResolve::Field>("m_IsTextBackingStringDirty");
        if (!backingStringDirtyField) {
            TextMeshProUGUI_Awake_Orig(self, method);
            UpdateFont(self);
            return;
        }
        bool centerAfterAwake = false;
        const bool backingStringWasDirty = Il2cppUtils::ClassGetFieldValue<bool>(self, backingStringDirtyField);
        const auto currText = get_Text_method->Invoke<UnityResolve::UnityType::String*>(self);
        Il2cppUtils::ClassSetFieldValue(self, backingStringDirtyField, backingStringWasDirty);
        if (currText) {
            //Log::InfoFmt("TextMeshProUGUI_Awake: %s", currText->ToString().c_str());
            std::string transText;
            bool hasTrans = Local::GetGenericText(currText->ToString(), &transText);
            std::string finalStr = MakeLocalizedText(currText->ToString(), transText, hasTrans);

            if (hasTrans || finalStr != currText->ToString() || HasTextLayoutMarker(finalStr)) {
                centerAfterAwake = ApplyTextLayoutMarkers(self, finalStr);
                if (Config::textTest) {
                    set_Text_method->Invoke<void>(self, UnityResolve::UnityType::String::New("[TA]" + finalStr));
                }
                else {
                    set_Text_method->Invoke<void>(self, UnityResolve::UnityType::String::New(finalStr));
                }
            }
        }

        TextMeshProUGUI_Awake_Orig(self, method);
        UpdateFont(self);
        if (centerAfterAwake) ApplyCenterTextLayout(self);
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

    // Legacy UnityEngine.UI.Text hook (non-TMP UI such as gifts/mail)
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

    // TMP_Text.SetCharArray(char[], int, int) — 선물함 설명 문구는 이 경로를 통해 설정합니다.
    DEFINE_HOOK(void, TMP_Text_SetCharArray, (void* self, void* charArray, int start, int count, void* mtd)) {
        if (charArray && start >= 0 && count > 0) {
            // IL2CPP char[] elements are uint16_t (UTF-16)
            auto arr = reinterpret_cast<UnityResolve::UnityType::Array<uint16_t>*>(charArray);
            // Bounds check: ensure start+count does not exceed array length
            if (static_cast<uintptr_t>(start + count) <= arr->max_length) {
                auto rawData = arr->GetData();
                if (rawData) {
                    // rawData is uintptr_t (byte address), each char16_t takes 2 bytes
                    // Must use start * sizeof(char16_t) instead of + start (otherwise offset is halved)
                    const std::u16string u16(
                        reinterpret_cast<const char16_t*>(rawData + static_cast<uintptr_t>(start) * sizeof(char16_t)),
                        static_cast<size_t>(count));
                    const std::string origText = Misc::ToUTF8(u16);
                    std::string phoneSubtitleText;
                    if (TryMakePhoneSubtitleTimerText(self, origText, &phoneSubtitleText)) {
                        UpdateFont(self);
                        TMP_Text_set_text_Orig(self, Il2cppString::New(phoneSubtitleText), nullptr);
                        UpdateFont(self);
                        return;
                    }

                    std::string transText;
                    bool hasTrans = Local::GetGenericText(origText, &transText);
                    std::string finalStr = MakeLocalizedText(origText, transText, hasTrans);

                    if (hasTrans || finalStr != origText || HasTextLayoutMarker(finalStr)) {
                        ApplyTextLayoutMarkers(self, finalStr);
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
                transText = FixLigature(std::move(transText));
                Local::DumpRemainingJapaneseText(transText);
                return TextField_set_value_Orig(self, UnityResolve::UnityType::String::New(transText));
            }
        }
        if (value) {
            Local::DumpRemainingJapaneseText(value->ToString());
        }
        TextField_set_value_Orig(self, value);
    }

    // Unused Hook
    DEFINE_HOOK(void, EffectGroup_ctor, (void* self, void* mtd)) {
        // auto self_klass = Il2cppUtils::get_class_from_instance(self);
        // Log::DebugFmt("EffectGroup_ctor: self: %s::%s", self_klass->namespaze, self_klass->name);
        EffectGroup_ctor_Orig(self, mtd);
    }

    // Used for MasterDB localization
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
        static auto is_native_object_alive = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Object", "IsNativeObjectAlive");
        if (!is_native_object_alive) return "";
        void* alive_args[] = {obj};
        void* exception = nullptr;
        auto alive_boxed = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke",
                is_native_object_alive->address, nullptr, alive_args, &exception);
        if (exception || !alive_boxed) return "";
        auto alive = UnityResolve::Invoke<uint8_t*>("il2cpp_object_unbox", alive_boxed);
        if (!alive || !*alive) return "";
        static auto get_name = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "get_name");
        if (!get_name) return "";
        exception = nullptr;
        auto nameStr = UnityResolve::Invoke<Il2cppString*>("il2cpp_runtime_invoke",
                get_name->address, obj, static_cast<void**>(nullptr), &exception);
        if (exception || !nameStr) return "";
        std::string name = nameStr->ToString();
        const std::string cloneSuffix = "(Clone)";
        if (name.length() >= cloneSuffix.length() && 
            name.compare(name.length() - cloneSuffix.length(), cloneSuffix.length(), cloneSuffix) == 0) {
            name = name.substr(0, name.length() - cloneSuffix.length());
        }
        return name;
    }

    void* GetAudioSourceClip(void* source) {
        if (!source || !IsUnityObjectAlive(source)) return nullptr;
        static auto get_clip = Il2cppUtils::GetMethod("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "get_clip");
        if (!get_clip) return nullptr;
        return get_clip->Invoke<void*>(source);
    }

    float GetAudioClipLength(void* clip) {
        if (!clip || !IsUnityObjectAlive(clip)) return 0.0f;
        static auto get_length = Il2cppUtils::GetMethod("UnityEngine.AudioModule.dll", "UnityEngine", "AudioClip", "get_length");
        if (!get_length) return 0.0f;
        return get_length->Invoke<float>(clip);
    }

    float GetAudioSourceTime(void* source) {
        if (!source || !IsUnityObjectAlive(source)) return -1.0f;
        static auto get_time = Il2cppUtils::GetMethod("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "get_time");
        if (!get_time) return -1.0f;
        return get_time->Invoke<float>(source);
    }
    void LogAudioDebug(const char* source, void* audioSource, void* clip, float volumeScale = -1.0f, float delay = -1.0f) {
        if (!Config::debugAudioLog) return;
        if (!clip) clip = GetAudioSourceClip(audioSource);

        const auto sourceName = GetObjectName(audioSource);
        const auto clipName = GetObjectName(clip);
        const auto clipLength = GetAudioClipLength(clip);
        Log::DebugFmt("ResourceDebug[Audio.%s]: source=%s clip=%s length=%.3f volume=%.3f delay=%.3f",
                      source,
                      sourceName.empty() ? "<unknown>" : sourceName.c_str(),
                      clipName.empty() ? "<none>" : clipName.c_str(),
                      clipLength,
                      volumeScale,
                      delay);
    }

    struct PhoneSubtitleLine {
        int line{};
        float time{};
        std::string text{};
    };

    std::unordered_map<std::string, std::vector<PhoneSubtitleLine>> phoneSubtitles{};
    bool phoneSubtitlesLoaded = false;

    struct ActivePhoneSubtitleState {
        bool active = false;
        void* source = nullptr;
        std::string clipName{};
        float startTime = 0.0f;
        float clipLength = 0.0f;
        int currentIndex = -1;
    } activePhoneSubtitle{};

    void* subtitleCanvasObject = nullptr;
    void* subtitleTextObject = nullptr;
    void* subtitleTextComponent = nullptr;

    std::string DecodeSubtitleText(std::string text) {
        size_t pos = 0;
        while ((pos = text.find("\\n", pos)) != std::string::npos) {
            text.replace(pos, 2, "\n");
            pos += 1;
        }
        return text;
    }

    void LoadPhoneSubtitles() {
        if (phoneSubtitlesLoaded) return;
        phoneSubtitlesLoaded = true;

        const auto subtitlePath = Local::GetBasePath() / "local-files" / "phoneSubtitles.json";
        try {
            if (!std::filesystem::exists(subtitlePath)) {
                Log::InfoFmt("PhoneSubtitle: %s not found", subtitlePath.string().c_str());
                return;
            }

            std::ifstream file(subtitlePath);
            std::string fileContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const auto jsonData = nlohmann::json::parse(fileContent);
            for (auto it = jsonData.begin(); it != jsonData.end(); ++it) {
                if (!it.value().is_array()) continue;

                std::vector<PhoneSubtitleLine> lines{};
                for (const auto& item : it.value()) {
                    if (!item.contains("time") || !item.contains("text")) continue;
                    PhoneSubtitleLine line{};
                    line.line = item.value("line", 0);
                    line.time = item.value("time", 0.0f);
                    line.text = DecodeSubtitleText(item.value("text", std::string{}));
                    if (!line.text.empty()) lines.emplace_back(std::move(line));
                }

                std::sort(lines.begin(), lines.end(), [](const PhoneSubtitleLine& a, const PhoneSubtitleLine& b) {
                    if (a.time == b.time) return a.line < b.line;
                    return a.time < b.time;
                });

                if (!lines.empty()) phoneSubtitles.emplace(it.key(), std::move(lines));
            }
            Log::InfoFmt("PhoneSubtitle: %zu clips loaded", phoneSubtitles.size());
        } catch (const std::exception& e) {
            Log::ErrorFmt("PhoneSubtitle: failed to load %s: %s", subtitlePath.string().c_str(), e.what());
        }
    }

    float GetRealtimeSinceStartup() {
        static auto get_realtimeSinceStartup = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Time", "get_realtimeSinceStartup");
        if (!get_realtimeSinceStartup) return 0.0f;
        return get_realtimeSinceStartup->Invoke<float>();
    }

    void* GetSystemTypeObject(const char* assemblyName, const char* nameSpaceName, const char* className) {
        auto klass = Il2cppUtils::GetClass(assemblyName, nameSpaceName, className);
        if (!klass) return nullptr;
        auto type = UnityResolve::Invoke<void*>("il2cpp_class_get_type", klass);
        if (!type) return nullptr;
        return UnityResolve::Invoke<void*>("il2cpp_type_get_object", type);
    }

    void* CreateGameObject(const std::string& name) {
        auto klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
        if (!klass) return nullptr;
        auto obj = UnityResolve::Invoke<void*>("il2cpp_object_new", klass);
        if (!obj) return nullptr;
        UnityResolve::UnityType::GameObject::Create(static_cast<UnityResolve::UnityType::GameObject*>(obj), name);
        return obj;
    }

    void* AddComponentByType(void* gameObject, const char* assemblyName, const char* nameSpaceName, const char* className) {
        if (!gameObject) return nullptr;
        static auto AddComponent = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject", "AddComponent", {"System.Type"});
        if (!AddComponent) return nullptr;
        auto typeObject = GetSystemTypeObject(assemblyName, nameSpaceName, className);
        if (!typeObject) return nullptr;
        return AddComponent->Invoke<void*>(gameObject, typeObject);
    }

    void* GetGameObjectTransform(void* gameObject) {
        if (!gameObject) return nullptr;
        static auto get_transform = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject", "get_transform");
        if (!get_transform) return nullptr;
        return get_transform->Invoke<void*>(gameObject);
    }

    void SetGameObjectActive(void* gameObject, bool active) {
        if (!gameObject || !IsUnityObjectAlive(gameObject)) return;
        static auto set_active = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject", "SetActive", {"System.Boolean"});
        if (set_active) set_active->Invoke<void>(gameObject, active);
    }

    void DontDestroyOnLoad(void* obj) {
        if (!obj) return;
        static auto DontDestroyOnLoadMethod = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "DontDestroyOnLoad", {"UnityEngine.Object"});
        if (DontDestroyOnLoadMethod) DontDestroyOnLoadMethod->Invoke<void>(obj);
    }

    void SetRectTransform(void* transform, UnityResolve::UnityType::Vector2 anchorMin,
                          UnityResolve::UnityType::Vector2 anchorMax, UnityResolve::UnityType::Vector2 pivot,
                          UnityResolve::UnityType::Vector2 anchoredPosition, UnityResolve::UnityType::Vector2 sizeDelta) {
        if (!transform) return;
        static auto set_anchorMin = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform", "set_anchorMin", {"UnityEngine.Vector2"});
        static auto set_anchorMax = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform", "set_anchorMax", {"UnityEngine.Vector2"});
        static auto set_pivot = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform", "set_pivot", {"UnityEngine.Vector2"});
        static auto set_anchoredPosition = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform", "set_anchoredPosition", {"UnityEngine.Vector2"});
        static auto set_sizeDelta = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform", "set_sizeDelta", {"UnityEngine.Vector2"});
        if (set_anchorMin) set_anchorMin->Invoke<void>(transform, anchorMin);
        if (set_anchorMax) set_anchorMax->Invoke<void>(transform, anchorMax);
        if (set_pivot) set_pivot->Invoke<void>(transform, pivot);
        if (set_anchoredPosition) set_anchoredPosition->Invoke<void>(transform, anchoredPosition);
        if (set_sizeDelta) set_sizeDelta->Invoke<void>(transform, sizeDelta);
    }

    bool EnsurePhoneSubtitleOverlay() {
        if (subtitleCanvasObject && IsUnityObjectAlive(subtitleCanvasObject) &&
            subtitleTextComponent && IsUnityObjectAlive(subtitleTextComponent)) {
            return true;
        }

        subtitleCanvasObject = CreateGameObject("HoshimiPhoneSubtitleCanvas");
        if (!subtitleCanvasObject) return false;
        auto canvas = AddComponentByType(subtitleCanvasObject, "UnityEngine.UIModule.dll", "UnityEngine", "Canvas");
        if (!canvas) return false;
        AddComponentByType(subtitleCanvasObject, "UnityEngine.UI.dll", "UnityEngine.UI", "CanvasScaler");
        AddComponentByType(subtitleCanvasObject, "UnityEngine.UI.dll", "UnityEngine.UI", "GraphicRaycaster");

        static auto Canvas_set_renderMode = Il2cppUtils::GetMethod("UnityEngine.UIModule.dll", "UnityEngine", "Canvas", "set_renderMode", {"UnityEngine.RenderMode"});
        static auto Canvas_set_sortingOrder = Il2cppUtils::GetMethod("UnityEngine.UIModule.dll", "UnityEngine", "Canvas", "set_sortingOrder", {"System.Int32"});
        if (Canvas_set_renderMode) Canvas_set_renderMode->Invoke<void>(canvas, 0);
        if (Canvas_set_sortingOrder) Canvas_set_sortingOrder->Invoke<void>(canvas, 32767);
        DontDestroyOnLoad(subtitleCanvasObject);

        subtitleTextObject = CreateGameObject("HoshimiPhoneSubtitleText");
        if (!subtitleTextObject) return false;
        subtitleTextComponent = AddComponentByType(subtitleTextObject, "Unity.TextMeshPro.dll", "TMPro", "TextMeshProUGUI");
        if (!subtitleTextComponent) return false;

        auto canvasTransform = GetGameObjectTransform(subtitleCanvasObject);
        auto textTransform = GetGameObjectTransform(subtitleTextObject);
        static auto Transform_SetParent = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "SetParent", {"UnityEngine.Transform", "System.Boolean"});
        if (Transform_SetParent && textTransform && canvasTransform) Transform_SetParent->Invoke<void>(textTransform, canvasTransform, false);

        SetRectTransform(textTransform,
                         UnityResolve::UnityType::Vector2(0.0f, 0.0f),
                         UnityResolve::UnityType::Vector2(1.0f, 0.0f),
                         UnityResolve::UnityType::Vector2(0.5f, 0.0f),
                         UnityResolve::UnityType::Vector2(0.0f, 86.0f),
                         UnityResolve::UnityType::Vector2(-160.0f, 190.0f));

        static auto set_fontSize = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_fontSize", {"System.Single"});
        static auto set_alignment = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_alignment", {"TMPro.TextAlignmentOptions"});
        static auto set_richText = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_richText", {"System.Boolean"});
        static auto set_color = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_color", {"UnityEngine.Color"});
        static auto set_enableWordWrapping = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_enableWordWrapping", {"System.Boolean"});
        static auto set_raycastTarget = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "Graphic", "set_raycastTarget", {"System.Boolean"});
        if (set_fontSize) set_fontSize->Invoke<void>(subtitleTextComponent, 30.0f);
        if (set_alignment) set_alignment->Invoke<void>(subtitleTextComponent, 514);
        if (set_color) set_color->Invoke<void>(subtitleTextComponent, UnityResolve::UnityType::Color(1.0f, 1.0f, 1.0f, 1.0f));
        if (set_enableWordWrapping) set_enableWordWrapping->Invoke<void>(subtitleTextComponent, true);
        if (set_raycastTarget) set_raycastTarget->Invoke<void>(subtitleTextComponent, false);
        UpdateFont(subtitleTextComponent);
        SetGameObjectActive(subtitleCanvasObject, false);
        return true;
    }

    void SetPhoneSubtitleText(const std::string& text) {
        if (!EnsurePhoneSubtitleOverlay()) return;
        static auto set_text = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_text");
        if (set_text) set_text->Invoke<void>(subtitleTextComponent, Il2cppString::New(text));
        SetGameObjectActive(subtitleCanvasObject, !text.empty());
    }

    void ClearPhoneSubtitle() {
        activePhoneSubtitle.active = false;
        activePhoneSubtitle.currentIndex = -1;
        currentPhoneSubtitleText.clear();
    }

    void TryStartPhoneSubtitle(void* audioSource, void* clip, float delaySeconds = 0.0f) {
        if (!Config::usePhoneSubtitles) {
            ClearPhoneSubtitle();
            return;
        }
        if (!clip) clip = GetAudioSourceClip(audioSource);
        const auto clipName = GetObjectName(clip);
        if (!clipName.starts_with("sud_vo_phone")) return;

        LoadPhoneSubtitles();
        auto it = phoneSubtitles.find(clipName);
        if (it == phoneSubtitles.end()) {
            if (Config::debugAudioLog) Log::DebugFmt("ResourceDebug[PhoneSubtitle]: clip=%s result=no_subtitle_data", clipName.c_str());
            ClearPhoneSubtitle();
            return;
        }

        activePhoneSubtitle.active = true;
        activePhoneSubtitle.source = audioSource;
        activePhoneSubtitle.clipName = clipName;
        activePhoneSubtitle.startTime = GetRealtimeSinceStartup() + std::max(0.0f, delaySeconds);
        activePhoneSubtitle.clipLength = GetAudioClipLength(clip);
        activePhoneSubtitle.currentIndex = -1;
        if (Config::debugAudioLog) Log::DebugFmt("ResourceDebug[PhoneSubtitle]: clip=%s result=start lines=%zu", clipName.c_str(), it->second.size());
    }

    void UpdatePhoneSubtitleOverlay() {
        if (!Config::usePhoneSubtitles) {
            if (activePhoneSubtitle.active || !currentPhoneSubtitleText.empty()) ClearPhoneSubtitle();
            return;
        }
        if (!activePhoneSubtitle.active) return;
        auto it = phoneSubtitles.find(activePhoneSubtitle.clipName);
        if (it == phoneSubtitles.end() || it->second.empty()) {
            ClearPhoneSubtitle();
            return;
        }

        const float realtimeElapsed = GetRealtimeSinceStartup() - activePhoneSubtitle.startTime;
        if (realtimeElapsed < 0.0f) {
            currentPhoneSubtitleText.clear();
            return;
        }

        if (activePhoneSubtitle.clipLength > 0.0f && realtimeElapsed > activePhoneSubtitle.clipLength + 1.5f) {
            ClearPhoneSubtitle();
            return;
        }

        const float audioElapsed = GetAudioSourceTime(activePhoneSubtitle.source);
        const float elapsed = audioElapsed >= 0.0f ? audioElapsed : realtimeElapsed;

        const auto& lines = it->second;
        int nextIndex = -1;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (lines[i].time <= elapsed) nextIndex = i;
            else break;
        }

        if (nextIndex < 0) {
            currentPhoneSubtitleText.clear();
            return;
        }
        if (nextIndex != activePhoneSubtitle.currentIndex) {
            activePhoneSubtitle.currentIndex = nextIndex;
            currentPhoneSubtitleText = lines[nextIndex].text;
        }
    }
    DEFINE_HOOK(void, AudioSource_Play, (void* self)) {
        LogAudioDebug("Play", self, nullptr);
        TryStartPhoneSubtitle(self, nullptr);
        AudioSource_Play_Orig(self);
    }

    DEFINE_HOOK(void, AudioSource_Play_UInt64, (void* self, uint64_t delay)) {
        LogAudioDebug("PlayUInt64", self, nullptr, -1.0f, static_cast<float>(delay));
        TryStartPhoneSubtitle(self, nullptr);
        AudioSource_Play_UInt64_Orig(self, delay);
    }

    DEFINE_HOOK(void, AudioSource_PlayDelayed, (void* self, float delay)) {
        LogAudioDebug("PlayDelayed", self, nullptr, -1.0f, delay);
        TryStartPhoneSubtitle(self, nullptr, delay);
        AudioSource_PlayDelayed_Orig(self, delay);
    }

    DEFINE_HOOK(void, AudioSource_PlayOneShot, (void* self, void* clip, float volumeScale)) {
        LogAudioDebug("PlayOneShot", self, clip, volumeScale);
        TryStartPhoneSubtitle(self, clip);
        AudioSource_PlayOneShot_Orig(self, clip, volumeScale);
    }

    DEFINE_HOOK(void, AudioSource_set_clip, (void* self, void* clip)) {
        LogAudioDebug("set_clip", self, clip);
        AudioSource_set_clip_Orig(self, clip);
    }
    void SetTextureClamp(void* texture) {
        static auto set_wrap_mode = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Texture", "set_wrapMode", {"UnityEngine.TextureWrapMode"});
        if (set_wrap_mode) set_wrap_mode->Invoke<void>(texture, 1);
    }

    void* CreateSpriteFromBytes(const std::vector<uint8_t>& bytes, void* originalSprite) {
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
        if (!load_success) return nullptr;
        SetTextureClamp(tex);

        static auto sprite_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Sprite");
        static auto sprite_create = sprite_klass->Get<UnityResolve::Method>("Create", {
                "UnityEngine.Texture2D", "UnityEngine.Rect", "UnityEngine.Vector2", "System.Single",
                "System.UInt32", "UnityEngine.SpriteMeshType", "UnityEngine.Vector4"});

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

            ImageDebugRect originalRect{};
            ImageDebugVector2 originalPivot{};
            ImageDebugVector4 border{};
            float pixelsPerUnit = 100.0f;
            if (originalSprite) {
                using GetRectInjected = void(*)(void*, ImageDebugRect*, void*);
                using GetVector2Injected = void(*)(void*, ImageDebugVector2*, void*);
                using GetVector4Injected = void(*)(void*, ImageDebugVector4*, void*);
                static auto getRect = reinterpret_cast<GetRectInjected>(Il2cppUtils::GetMethodPointer(
                        "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_rect_Injected", {"UnityEngine.Rect&"}));
                static auto getPivot = reinterpret_cast<GetVector2Injected>(Il2cppUtils::GetMethodPointer(
                        "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_pivot_Injected", {"UnityEngine.Vector2&"}));
                static auto getBorder = reinterpret_cast<GetVector4Injected>(Il2cppUtils::GetMethodPointer(
                        "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_border_Injected", {"UnityEngine.Vector4&"}));
                static auto getPixelsPerUnit = Il2cppUtils::GetMethod(
                        "UnityEngine.CoreModule.dll", "UnityEngine", "Sprite", "get_pixelsPerUnit");
                if (getRect) getRect(originalSprite, &originalRect, nullptr);
                if (getPivot) getPivot(originalSprite, &originalPivot, nullptr);
                if (getBorder) getBorder(originalSprite, &border, nullptr);
                if (getPixelsPerUnit) pixelsPerUnit = getPixelsPerUnit->Invoke<float>(originalSprite);
            }

            ImageDebugRect rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
            ImageDebugVector2 pivot{0.5f, 0.5f};
            if (originalRect.width > 0.0f && originalRect.height > 0.0f) {
                pivot.x = originalPivot.x / originalRect.width;
                pivot.y = originalPivot.y / originalRect.height;
            }
            uint32_t extrude = 0;
            int32_t meshType = 0;
            void* args[7] = {tex, &rect, &pivot, &pixelsPerUnit, &extrude, &meshType, &border};
            auto sprite = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", sprite_create->address, nullptr, args, nullptr);
            MarkReplacementAssetPersistent(sprite);
            return sprite;
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
        if (load_success) SetTextureClamp(tex);
        return load_success ? tex : nullptr;
    }

    void SetImagePreserveAspect(void* self) {
        if (!Config::forceImagePreserveAspect) return;
        static auto set_preserve_aspect = Il2cppUtils::GetMethod("UnityEngine.UI.dll", "UnityEngine.UI", "Image", "set_preserveAspect", {"System.Boolean"});
        if (set_preserve_aspect) set_preserve_aspect->Invoke<void>(self, true);
    }

    bool LoadReplacementImageBytes(const std::string& name, std::vector<uint8_t>* bytes) {
        if (name.empty()) return false;
        return Local::GetResourceBytes(name + ".png", bytes) || Local::GetResourceBytes(name, bytes);
    }

    void* GetOrCreateReplacementSprite(const std::string& name, void* originalSprite, bool rememberMiss) {
        if (name.empty()) return nullptr;
        if (rememberMiss && sprite_negative_cache.contains(name)) return nullptr;
        if (auto cachedSprite = GetAliveCachedAsset(sprite_cache, name)) return cachedSprite;

        std::vector<uint8_t> bytes;
        if (!LoadReplacementImageBytes(name, &bytes)) {
            if (rememberMiss) sprite_negative_cache.insert(name);
            return nullptr;
        }

        auto newSprite = CreateSpriteFromBytes(bytes, originalSprite);
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
            if (auto replacementSprite = GetOrCreateReplacementSprite(name, sprite, true)) {
                LogImageResourceDebug("Graphic.OnEnable.Image", self, name, true);
                LogSpriteReplacementDebug("Graphic.OnEnable.Image", self, name, sprite, replacementSprite);
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
                if (auto replacementSprite = GetOrCreateReplacementSprite(name, value, false)) {
                    LogImageResourceDebug("Image.set_sprite", self, name, true);
                    LogSpriteReplacementDebug("Image.set_sprite", self, name, value, replacementSprite);
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
                if (auto replacementSprite = GetOrCreateReplacementSprite(name, value, false)) {
                    LogImageResourceDebug("Image.set_overrideSprite", self, name, true);
                    LogSpriteReplacementDebug("Image.set_overrideSprite", self, name, value, replacementSprite);
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
        PhotoPoseId
    };

    bool HasOctoAssetBundle(const std::string& bundleNameStr) {
        static const auto isAssetBundleExists = Il2cppUtils::GetMethod("Octo.dll",
                "Octo", "OctoManager", "IsAssetBundleExists");
        if (!isAssetBundleExists || bundleNameStr.empty()) return false;
        auto bundleName = Il2cppString::New(bundleNameStr);
        return bundleName && isAssetBundleExists->Invoke<bool>(bundleName, isAssetBundleExists->address);
    }

    bool CostumeHas3DBodyAsset(void* costume) {
        if (!costume) return false;
        static auto Costume_get_Id = Il2cppUtils::GetMethod(
                "Assembly-CSharp.dll", "Solis.Common.Proto.Master", "Costume", "get_Id");
        static auto Costume_get_BodyAssetId = Il2cppUtils::GetMethod(
                "Assembly-CSharp.dll", "Solis.Common.Proto.Master", "Costume", "get_BodyAssetId");
        static auto Costume_GetBodyAssetName = Il2cppUtils::GetMethod(
                "Assembly-CSharp.dll", "Solis.Common.Proto.Master", "Costume", "GetBodyAssetName");
        if (!Costume_get_BodyAssetId || !Costume_GetBodyAssetName) return true;

        std::string costumeIdStr;
        if (Costume_get_Id) {
            auto idObj = Costume_get_Id->Invoke<Il2cppString*>(costume);
            if (idObj) costumeIdStr = idObj->ToString();
        }

        static std::mutex cacheMutex;
        static std::unordered_map<std::string, bool> body3DCache;
        static std::atomic<bool> octoVerified{false};
        if (!costumeIdStr.empty()) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = body3DCache.find(costumeIdStr);
            if (it != body3DCache.end()) return it->second;
        }

        auto bodyAssetId = Costume_get_BodyAssetId->Invoke<Il2cppString*>(costume);
        bool has3D = false;
        std::string bodyAssetNameStr;
        if (bodyAssetId && !bodyAssetId->ToString().empty()) {
            auto bodyAssetName = Costume_GetBodyAssetName->Invoke<Il2cppString*>(costume);
            if (bodyAssetName) {
                bodyAssetNameStr = bodyAssetName->ToString();
                if (!bodyAssetNameStr.empty()) {
                    has3D = HasOctoAssetBundle(bodyAssetNameStr);
                }
            }
        }
        if (has3D) {
            octoVerified.store(true, std::memory_order_relaxed);
        }

        if (!costumeIdStr.empty() && (has3D || bodyAssetNameStr.empty() || octoVerified.load(std::memory_order_relaxed))) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            body3DCache.emplace(costumeIdStr, has3D);
        }
        if (!has3D) {
            Log::InfoFmt("Costume 3D filter: hiding SD-only costume=%s bodyAsset=%s",
                         costumeIdStr.c_str(), bodyAssetNameStr.c_str());
        }
        return has3D;
    }

    bool IsSdScheduleCostumeSpecifier(void* costumeSpecifier) {
        if (!costumeSpecifier) return false;
        auto klass = Il2cppUtils::get_class_from_instance(costumeSpecifier);
        if (!klass || !klass->name) return false;
        std::string name(klass->name);
        return name == "Promotion" || name == "FanEvent" || name == "RefreshLevel";
    }

    thread_local bool g_filterCostumesTo3DOnly = false;
    std::atomic<bool> g_activeCostumeSheetAllowsSdOnly{false};

    struct Scoped3DCostumeFilter {
        bool prev;
        explicit Scoped3DCostumeFilter(bool enable) : prev(g_filterCostumesTo3DOnly) {
            g_filterCostumesTo3DOnly = enable;
        }
        ~Scoped3DCostumeFilter() {
            g_filterCostumesTo3DOnly = prev;
        }
    };

    std::vector<std::string> GetSolisMasterIdAll(SolisMasterIdType getType = SolisMasterIdType::CostumeId,
                                                 const std::string& characterId = "",
                                                 bool only3DCostumes = false) {
        std::vector<std::string> ret{};

        const char* masterGetterName = getType == SolisMasterIdType::CostumeId ? "get_CostumeMaster" :
                                       getType == SolisMasterIdType::HairId ? "get_HairMaster" : "get_PhotoPoseMaster";
        const char* masterClassName = getType == SolisMasterIdType::CostumeId ? "CostumeMaster" :
                                      getType == SolisMasterIdType::HairId ? "HairMaster" : "PhotoPoseMaster";
        const char* protoClassName = getType == SolisMasterIdType::CostumeId ? "Costume" :
                                     getType == SolisMasterIdType::HairId ? "Hair" : "PhotoPose";

        static auto get_CostumeMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                               "MasterManager", "get_CostumeMaster");
        static auto get_HairMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                            "MasterManager", "get_HairMaster");
        static auto get_PhotoPoseMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                 "MasterManager", "get_PhotoPoseMaster");
        static auto CostumeMaster_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Master",
                                                                               "CostumeMaster", "GetAllWithSortByKey",
                                                                               {"Solis.Common.Master.CostumeSortType"});
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
        static auto Hair_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                         "Hair", "get_Id");
        static auto Hair_get_CharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                                  "Hair", "get_CharacterId");
        static auto PhotoPose_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Master",
                                                              "PhotoPose", "get_Id");

        auto getMaster = getType == SolisMasterIdType::CostumeId ? get_CostumeMaster :
                         getType == SolisMasterIdType::HairId ? get_HairMaster : get_PhotoPoseMaster;
        auto getAllWithSortByKey = getType == SolisMasterIdType::CostumeId ? CostumeMaster_GetAllWithSortByKey :
                                   getType == SolisMasterIdType::HairId ? HairMaster_GetAllWithSortByKey : PhotoPoseMaster_GetAllWithSortByKey;
        auto getId = getType == SolisMasterIdType::CostumeId ? Costume_get_Id :
                     getType == SolisMasterIdType::HairId ? Hair_get_Id : PhotoPose_get_Id;
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
        std::unordered_set<std::string> seenIds{};
        const auto masterCount = listEditor.get_Count();
        if (masterCount > 0) {
            seenIds.reserve(static_cast<size_t>(masterCount));
            ret.reserve(static_cast<size_t>(masterCount));
        }
        for (auto item : listEditor) {
            if (!item) continue;

            if (getType == SolisMasterIdType::CostumeId) {
                if (!characterId.empty() && Costume_get_CharacterId) {
                    auto itemCharacterId = Costume_get_CharacterId->Invoke<Il2cppString*>(item);
                    if (!itemCharacterId || itemCharacterId->ToString() != characterId) continue;
                }
                if (only3DCostumes && !CostumeHas3DBodyAsset(item)) continue;
            }
            else if (getType == SolisMasterIdType::HairId) {
                if (!characterId.empty() && Hair_get_CharacterId) {
                    auto itemCharacterId = Hair_get_CharacterId->Invoke<Il2cppString*>(item);
                    if (!itemCharacterId || itemCharacterId->ToString() != characterId) continue;
                }
            }

            auto id = getId->Invoke<Il2cppString*>(item);
            if (!id) continue;
            auto idStr = id->ToString();
            if (!idStr.empty() && seenIds.emplace(idStr).second) {
                ret.emplace_back(std::move(idStr));
            }
        }

        return ret;
    }

    bool IsSolisMasterIdExists(const std::string& id, SolisMasterIdType getType) {
        if (id.empty()) return false;
        static std::mutex cacheMutex;
        static std::unordered_set<std::string> costumeIds{};
        static std::unordered_set<std::string> hairIds{};
        static std::unordered_set<std::string> photoPoseIds{};

        auto& ids = getType == SolisMasterIdType::CostumeId ? costumeIds :
                    getType == SolisMasterIdType::HairId ? hairIds : photoPoseIds;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (!ids.empty()) {
                return ids.contains(id);
            }
        }
        auto allIds = GetSolisMasterIdAll(getType);
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (ids.empty()) {
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
        static auto UserCostume_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                              "UserCostume", ".ctor");
        static auto UserHair_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                           "UserHair", ".ctor");
        static auto UserPhotoPose_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                "UserPhotoPose", ".ctor");

        static auto UserCostume_set_Checked = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                     "UserCostume", "set_Checked");
        static auto UserHair_set_Checked = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Common.Proto.Transaction",
                                                                  "UserHair", "set_Checked");

        auto className = std::string(userClassName);
        auto klass = className == "UserCostume" ? UserCostume_klass :
                     className == "UserHair" ? UserHair_klass : UserPhotoPose_klass;
        auto ctor = className == "UserCostume" ? UserCostume_ctor :
                    className == "UserHair" ? UserHair_ctor : UserPhotoPose_ctor;
        auto set_Checked = className == "UserCostume" ? UserCostume_set_Checked :
                           className == "UserHair" ? UserHair_set_Checked : nullptr;

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
            if (set_Checked) {
                set_Checked->Invoke<void>(userData, true);
            }
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

        auto allIds = GetSolisMasterIdAll(SolisMasterIdType::CostumeId, characterId, g_filterCostumesTo3DOnly);
        if (Config::unlockCrossCharacterCostume && !characterId.empty()) {
            auto crossIds = GetSolisMasterIdAll(SolisMasterIdType::CostumeId, "", g_filterCostumesTo3DOnly);
            allIds.reserve(allIds.size() + crossIds.size());
            allIds.insert(allIds.end(), std::make_move_iterator(crossIds.begin()),
                          std::make_move_iterator(crossIds.end()));
        }
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

    void* GetEmptySolisPhotoPoseTypes() {
        static std::mutex mutex;
        static uint32_t emptyPoseTypesHandle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (emptyPoseTypesHandle) {
                if (auto target = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", emptyPoseTypesHandle)) {
                    return target;
                }
            }
        }

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

        auto obj = UnityResolve::Invoke<void*>("il2cpp_object_new", RepeatedField_PhotoPoseType_klass);
        if (!obj) return nullptr;
        RepeatedField_PhotoPoseType_ctor(obj, RepeatedField_PhotoPoseType_ctor_mtd);

        std::lock_guard<std::mutex> lock(mutex);
        if (!emptyPoseTypesHandle) {
            emptyPoseTypesHandle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new", obj, false);
        }
        return emptyPoseTypesHandle
            ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", emptyPoseTypesHandle)
            : obj;
    }

    DEFINE_HOOK(void, LiveUtility_LoadLiveScene_FullSkip, (void* data, bool isFullSkip, void* method)) {
        if (Config::skipLiveToResult && data && LiveUtility_LoadLiveResult_Call) {
            Log::InfoFmt("LiveSkip: skip live scene and load result, isFullSkip=%d", isFullSkip ? 1 : 0);
            LiveUtility_LoadLiveResult_Call(data, nullptr);
            return;
        }
        return LiveUtility_LoadLiveScene_FullSkip_Orig(data, isFullSkip, method);
    }

    thread_local void* activePhotoTransition = nullptr;
    thread_local void* activePhotoOwnedOutfit = nullptr;

    struct PhotoAppearanceEntry {
        uint32_t ownedHandle;
        uint32_t selectedHandle;
        std::string characterId;
    };

    std::mutex photoAppearanceMutex;
    std::vector<PhotoAppearanceEntry> photoAppearanceEntries;

    bool PhotoOwnedEntryEnabled() {
        return Config::unlockAllLiveCostume;
    }

    void ReleasePhotoAppearanceEntry(const PhotoAppearanceEntry& entry) {
        if (entry.ownedHandle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", entry.ownedHandle);
        if (entry.selectedHandle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", entry.selectedHandle);
    }

    int GetUnboxedStructFieldOffset(void* klass, const char* fieldName) {
        if (!klass || !fieldName) return -1;
        auto field = UnityResolve::Invoke<void*>("il2cpp_class_get_field_from_name", klass, fieldName);
        if (!field) return -1;
        const auto offset = UnityResolve::Invoke<size_t>("il2cpp_field_get_offset", field);
        constexpr size_t objectHeaderSize = 2 * sizeof(void*);
        return offset >= objectHeaderSize ? static_cast<int>(offset - objectHeaderSize) : -1;
    }

    template<typename T>
    T ReadUnboxedStructField(const void* structPtr, int offset) {
        T value{};
        if (!structPtr || offset < 0) return value;
        std::memcpy(&value, static_cast<const uint8_t*>(structPtr) + offset, sizeof(T));
        return value;
    }

    void* ReadManagedObjectField(void* obj, const char* fieldName) {
        if (!obj || !fieldName) return nullptr;
        auto klass = Il2cppUtils::get_class_from_instance(obj);
        if (!klass) return nullptr;
        auto field = UnityResolve::Invoke<void*>("il2cpp_class_get_field_from_name", klass, fieldName);
        if (!field) return nullptr;
        void* value = nullptr;
        UnityResolve::Invoke<void>("il2cpp_field_get_value", obj, field, &value);
        return value;
    }

    void* GetActorCostumeCostume(void* actorCostume) {
        if (!actorCostume) return nullptr;
        static const auto getCostume = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Data", "ActorCostume", "get_Costume");
        return getCostume ? getCostume->Invoke<void*>(actorCostume, getCostume->address) : nullptr;
    }

    void* GetActorCostumeHair(void* actorCostume) {
        if (!actorCostume) return nullptr;
        static const auto getHair = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Data", "ActorCostume", "get_Hair");
        return getHair ? getHair->Invoke<void*>(actorCostume, getHair->address) : nullptr;
    }

    std::string GetCostumeCharacterId(void* costume) {
        if (!costume) return {};
        static const auto getCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Proto.Master", "Costume", "get_CharacterId");
        auto id = getCharacterId ? getCharacterId->Invoke<Il2cppString*>(costume, getCharacterId->address) : nullptr;
        return id ? id->ToString() : std::string{};
    }

    std::string GetUserCharacterId(void* userCharacter) {
        if (!userCharacter) return {};
        static const auto getCharacterId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Proto.Transaction", "UserCharacter", "get_CharacterId");
        auto id = getCharacterId ? getCharacterId->Invoke<Il2cppString*>(userCharacter, getCharacterId->address) : nullptr;
        return id ? id->ToString() : std::string{};
    }

    std::string GetCharacterMasterId(void* character) {
        if (!character) return {};
        static const auto getId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Proto.Master", "Character", "get_Id");
        auto id = getId ? getId->Invoke<Il2cppString*>(character, getId->address) : nullptr;
        return id ? id->ToString() : std::string{};
    }

    void* GetPresenterSelectDataModel(void* presenter) {
        if (!presenter) return nullptr;
        auto klass = Il2cppUtils::get_class_from_instance(presenter);
        auto mtd = klass ? Il2cppUtils::il2cpp_class_get_method_from_name(klass, "get_SelectDataModel", 0) : nullptr;
        if (!mtd || !mtd->methodPointer) return nullptr;
        return reinterpret_cast<void* (*)(void*, const Il2cppUtils::MethodInfo*)>(mtd->methodPointer)(presenter, mtd);
    }

    void* GetSelectDataModelActivity(void* selectDataModel) {
        if (!selectDataModel) return nullptr;
        static const auto getActivity = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.OutGame", "PhotographySelectDataModel", "get_Activity");
        return getActivity ? getActivity->Invoke<void*>(selectDataModel, getActivity->address) : nullptr;
    }

    void* GetSelectDataModelExpression(void* selectDataModel) {
        if (!selectDataModel) return nullptr;
        static const auto getExpression = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.OutGame", "PhotographySelectDataModel", "get_Expression");
        return getExpression ? getExpression->Invoke<void*>(selectDataModel, getExpression->address) : nullptr;
    }

    void RememberPhotoAppearance(const std::string& characterId, void* ownedOutfit, void* selectedOutfit) {
        if (characterId.empty() || !ownedOutfit || !selectedOutfit) return;

        std::lock_guard lock(photoAppearanceMutex);
        for (auto entry = photoAppearanceEntries.begin(); entry != photoAppearanceEntries.end();) {
            auto target = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", entry->ownedHandle);
            if (!target) {
                ReleasePhotoAppearanceEntry(*entry);
                entry = photoAppearanceEntries.erase(entry);
                continue;
            }
            if (target == ownedOutfit) {
                if (entry->selectedHandle) {
                    UnityResolve::Invoke<void>("il2cpp_gchandle_free", entry->selectedHandle);
                }
                entry->selectedHandle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new", selectedOutfit, false);
                entry->characterId = characterId;
                return;
            }
            ++entry;
        }

        const auto ownedHandle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new_weakref", ownedOutfit, false);
        const auto selectedHandle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new", selectedOutfit, false);
        if (!ownedHandle || !selectedHandle) {
            if (ownedHandle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", ownedHandle);
            if (selectedHandle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", selectedHandle);
            return;
        }
        constexpr size_t maxObservedOutfits = 64;
        if (photoAppearanceEntries.size() >= maxObservedOutfits) {
            ReleasePhotoAppearanceEntry(photoAppearanceEntries.front());
            photoAppearanceEntries.erase(photoAppearanceEntries.begin());
        }
        photoAppearanceEntries.push_back({ownedHandle, selectedHandle, characterId});
    }

    void* FindPhotoAppearance(void* ownedOutfit) {
        if (!ownedOutfit) return nullptr;
        std::lock_guard lock(photoAppearanceMutex);
        for (auto entry = photoAppearanceEntries.begin(); entry != photoAppearanceEntries.end();) {
            auto target = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", entry->ownedHandle);
            if (!target) {
                ReleasePhotoAppearanceEntry(*entry);
                entry = photoAppearanceEntries.erase(entry);
                continue;
            }
            if (target == ownedOutfit) {
                return entry->selectedHandle
                    ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", entry->selectedHandle) : nullptr;
            }
            ++entry;
        }
        return nullptr;
    }

    struct ActorCostumeOwnerEntry {
        uint32_t actorCostumeHandle = 0;
        std::string characterId;
    };
    std::mutex actorCostumeOwnerMutex;
    std::vector<ActorCostumeOwnerEntry> actorCostumeOwnerEntries;

    void RememberActorCostumeOwner(void* actorCostume, const std::string& characterId) {
        if (!actorCostume || characterId.empty()) return;
        std::lock_guard lock(actorCostumeOwnerMutex);
        for (auto it = actorCostumeOwnerEntries.begin(); it != actorCostumeOwnerEntries.end();) {
            auto target = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", it->actorCostumeHandle);
            if (!target) {
                UnityResolve::Invoke<void>("il2cpp_gchandle_free", it->actorCostumeHandle);
                it = actorCostumeOwnerEntries.erase(it);
                continue;
            }
            if (target == actorCostume) {
                it->characterId = characterId;
                return;
            }
            ++it;
        }
        const auto handle = UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new_weakref", actorCostume, false);
        if (!handle) return;
        constexpr size_t maxTrackedOwners = 128;
        if (actorCostumeOwnerEntries.size() >= maxTrackedOwners) {
            UnityResolve::Invoke<void>("il2cpp_gchandle_free", actorCostumeOwnerEntries.front().actorCostumeHandle);
            actorCostumeOwnerEntries.erase(actorCostumeOwnerEntries.begin());
        }
        actorCostumeOwnerEntries.push_back({handle, characterId});
    }

    std::string FindActorCostumeOwner(void* actorCostume) {
        if (!actorCostume) return {};
        std::lock_guard lock(actorCostumeOwnerMutex);
        for (auto it = actorCostumeOwnerEntries.begin(); it != actorCostumeOwnerEntries.end();) {
            auto target = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", it->actorCostumeHandle);
            if (!target) {
                UnityResolve::Invoke<void>("il2cpp_gchandle_free", it->actorCostumeHandle);
                it = actorCostumeOwnerEntries.erase(it);
                continue;
            }
            if (target == actorCostume) {
                return it->characterId;
            }
            ++it;
        }
        return {};
    }

    void SetActorDescriptorNameFromCharacterId(void* descriptor, const std::string& characterId) {
        if (!descriptor || characterId.empty()) return;
        const std::string actorName = characterId.starts_with("char-") ? characterId.substr(5) : characterId;
        if (actorName.empty()) return;
        auto klass = Il2cppUtils::get_class_from_instance(descriptor);
        auto setName = klass ? Il2cppUtils::il2cpp_class_get_method_from_name(klass, "set_name", 1) : nullptr;
        if (setName && setName->methodPointer) {
            reinterpret_cast<void (*)(void*, Il2cppString*, const Il2cppUtils::MethodInfo*)>(setName->methodPointer)(
                    descriptor, Il2cppString::New(actorName), setName);
        }
    }

    void* ReplaceWithOwnedPhotoEntryOutfit(const std::string& characterId, void* selectedOutfit,
                                           void* costumeSpecifier = nullptr) {
        if (!PhotoOwnedEntryEnabled() || characterId.empty() || !selectedOutfit) return selectedOutfit;
        auto owned = PhotoRequests::FindOwnedPhotoEntryOutfit(characterId, selectedOutfit, costumeSpecifier);
        if (!owned) return selectedOutfit;
        RememberPhotoAppearance(characterId, owned, selectedOutfit);
        return owned;
    }

    void ReplaceActorCostumeListInPlace(void* actorCostumeList, void* costumeSpecifier = nullptr) {
        if (!PhotoOwnedEntryEnabled() || !actorCostumeList) return;
        Il2cppUtils::Tools::CSListEditor<void*> list(actorCostumeList);
        const int count = list.get_Count();
        if (count <= 0 || count > 32) return;
        for (int index = 0; index < count; ++index) {
            auto selected = list.get_Item(index);
            if (!selected) continue;
            auto characterId = FindActorCostumeOwner(selected);
            if (characterId.empty()) {
                characterId = GetCostumeCharacterId(GetActorCostumeCostume(selected));
            }
            if (characterId.empty()) continue;
            auto owned = ReplaceWithOwnedPhotoEntryOutfit(characterId, selected, costumeSpecifier);
            if (owned && owned != selected) {
                list.set_Item(index, owned);
            }
        }
    }

    void* ResolveAppearanceFromTransition(void* transition, void* costume) {
        if (!transition || !costume) return nullptr;
        static const auto getOutfits = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Photography.Data", "AbstPhotographyTransitionData", "get_ActorCostumeList");
        static const auto getDisplayOutfits = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Photography.Data", "AbstPhotographyTransitionData", "get_DisplayActorCostumeList");
        if (!getOutfits) return nullptr;

        auto outfitsObj = getOutfits->Invoke<void*>(transition, getOutfits->address);
        if (!outfitsObj) return nullptr;
        Il2cppUtils::Tools::CSListEditor<void*> outfits(outfitsObj);
        const int count = outfits.get_Count();
        if (count <= 0 || count > 32) return nullptr;

        for (int i = 0; i < count; ++i) {
            auto entryOutfit = outfits.get_Item(i);
            if (entryOutfit && GetActorCostumeCostume(entryOutfit) == costume) {
                if (auto selected = FindPhotoAppearance(entryOutfit)) return selected;
            }
        }

        if (getDisplayOutfits) {
            auto displayObj = getDisplayOutfits->Invoke<void*>(transition, getDisplayOutfits->address);
            if (displayObj) {
                Il2cppUtils::Tools::CSListEditor<void*> display(displayObj);
                if (display.get_Count() == count) {
                    for (int i = 0; i < count; ++i) {
                        auto displayOutfit = display.get_Item(i);
                        if (displayOutfit && GetActorCostumeCostume(displayOutfit) == costume) {
                            auto entryOutfit = outfits.get_Item(i);
                            if (auto selected = FindPhotoAppearance(entryOutfit)) {
                                return selected;
                            }
                        }
                    }
                }
            }
        }
        return nullptr;
    }

    struct ScopedPhotoTransition {
        void* previous;
        explicit ScopedPhotoTransition(void* transition) : previous(activePhotoTransition) {
            activePhotoTransition = transition;
        }
        ~ScopedPhotoTransition() { activePhotoTransition = previous; }
    };

    struct ScopedPhotoOwnedOutfit {
        void* previous;
        explicit ScopedPhotoOwnedOutfit(void* outfit) : previous(activePhotoOwnedOutfit) {
            activePhotoOwnedOutfit = outfit;
        }
        ~ScopedPhotoOwnedOutfit() { activePhotoOwnedOutfit = previous; }
    };

    // 1. Live Photography confirm entry
    DEFINE_HOOK(void*, Solis_Photography_ResolveConfirmOutfit,
        (void* self, void* userCharacter, void* mtd)) {
        auto selected = Solis_Photography_ResolveConfirmOutfit_Orig(self, userCharacter, mtd);
        return ReplaceWithOwnedPhotoEntryOutfit(GetUserCharacterId(userCharacter), selected, nullptr);
    }

    // 2. Activity Photography confirm entry
    DEFINE_HOOK(void*, Solis_ActivityPhotography_ResolveConfirmOutfit,
        (void* self, void* userCharacter, void* mtd)) {
        auto selected = Solis_ActivityPhotography_ResolveConfirmOutfit_Orig(self, userCharacter, mtd);
        if (!PhotoOwnedEntryEnabled() || !selected) return selected;
        auto presenter = ReadManagedObjectField(self, "<>4__this");
        auto specifier = GetSelectDataModelActivity(GetPresenterSelectDataModel(presenter));
        return ReplaceWithOwnedPhotoEntryOutfit(GetUserCharacterId(userCharacter), selected, specifier);
    }

    // 3. Special Live Photography confirm entry
    DEFINE_HOOK(void*, Solis_SpecialLivePhotography_ResolveConfirmOutfit,
        (void* self, void* character, void* mtd)) {
        auto selected = Solis_SpecialLivePhotography_ResolveConfirmOutfit_Orig(self, character, mtd);
        return ReplaceWithOwnedPhotoEntryOutfit(GetCharacterMasterId(character), selected, nullptr);
    }

    // 4. Special Activity Photography confirm entry
    DEFINE_HOOK(void*, Solis_SpecialActivityPhotography_ResolveConfirmOutfit,
        (void* self, void* character, void* mtd)) {
        auto selected = Solis_SpecialActivityPhotography_ResolveConfirmOutfit_Orig(self, character, mtd);
        if (!PhotoOwnedEntryEnabled() || !selected) return selected;
        auto presenter = ReadManagedObjectField(self, "<>4__this");
        auto specifier = GetSelectDataModelActivity(GetPresenterSelectDataModel(presenter));
        return ReplaceWithOwnedPhotoEntryOutfit(GetCharacterMasterId(character), selected, specifier);
    }

    // 5. Panorama 360 Live Photography confirm entry
    DEFINE_HOOK(void*, Solis_PanoramaPhotography_ResolveConfirmOutfit,
        (void* self, void* userCharacter, void* mtd)) {
        auto selected = Solis_PanoramaPhotography_ResolveConfirmOutfit_Orig(self, userCharacter, mtd);
        return ReplaceWithOwnedPhotoEntryOutfit(GetUserCharacterId(userCharacter), selected, nullptr);
    }

    void* g_expressionConfirmStateMachineClass = nullptr;
    void* g_contestLiveCheckStateMachineClass = nullptr;
    void* g_contestActivityCheckStateMachineClass = nullptr;
    void* g_spotInitStateMachineClass = nullptr;
    void* g_liveModeInitStateMachineClass = nullptr;

    // 6. Expression Photography confirm entry
    DEFINE_HOOK(void, Solis_ExpressionPhotography_OnExecuteConfirmMoveNext, (void* self, void* mtd)) {
        if (PhotoOwnedEntryEnabled() && self && g_expressionConfirmStateMachineClass) {
            static const int stateOffset = GetUnboxedStructFieldOffset(g_expressionConfirmStateMachineClass, "<>1__state");
            static const int thisOffset = GetUnboxedStructFieldOffset(g_expressionConfirmStateMachineClass, "<>4__this");
            static const int costumesOffset = GetUnboxedStructFieldOffset(g_expressionConfirmStateMachineClass, "actorCostumes");
            if (ReadUnboxedStructField<int>(self, stateOffset) == -1) {
                auto presenter = ReadUnboxedStructField<void*>(self, thisOffset);
                auto actorCostumes = ReadUnboxedStructField<void*>(self, costumesOffset);
                auto specifier = GetSelectDataModelExpression(GetPresenterSelectDataModel(presenter));
                ReplaceActorCostumeListInPlace(actorCostumes, specifier);
            }
        }
        Solis_ExpressionPhotography_OnExecuteConfirmMoveNext_Orig(self, mtd);
    }

    // 7. Photo Contest Live confirm entry
    DEFINE_HOOK(void, Solis_PhotoContestLive_CheckShootingMoveNext, (void* self, void* mtd)) {
        if (PhotoOwnedEntryEnabled() && self && g_contestLiveCheckStateMachineClass) {
            static const int stateOffset = GetUnboxedStructFieldOffset(g_contestLiveCheckStateMachineClass, "<>1__state");
            static const int costumesOffset = GetUnboxedStructFieldOffset(g_contestLiveCheckStateMachineClass, "actorCostumes");
            if (ReadUnboxedStructField<int>(self, stateOffset) == -1) {
                auto actorCostumes = ReadUnboxedStructField<void*>(self, costumesOffset);
                ReplaceActorCostumeListInPlace(actorCostumes, nullptr);
            }
        }
        Solis_PhotoContestLive_CheckShootingMoveNext_Orig(self, mtd);
    }

    // 8. Photo Contest Activity confirm entry
    DEFINE_HOOK(void, Solis_PhotoContestActivity_CheckShootingMoveNext, (void* self, void* mtd)) {
        if (PhotoOwnedEntryEnabled() && self && g_contestActivityCheckStateMachineClass) {
            static const int stateOffset = GetUnboxedStructFieldOffset(g_contestActivityCheckStateMachineClass, "<>1__state");
            static const int activityOffset = GetUnboxedStructFieldOffset(g_contestActivityCheckStateMachineClass, "activity");
            static const int costumesOffset = GetUnboxedStructFieldOffset(g_contestActivityCheckStateMachineClass, "actorCostumes");
            if (ReadUnboxedStructField<int>(self, stateOffset) == -1) {
                auto activity = ReadUnboxedStructField<void*>(self, activityOffset);
                auto actorCostumes = ReadUnboxedStructField<void*>(self, costumesOffset);
                ReplaceActorCostumeListInPlace(actorCostumes, activity);
            }
        }
        Solis_PhotoContestActivity_CheckShootingMoveNext_Orig(self, mtd);
    }

    // Asset download scope: Live / Contest Live / Special Live / Panorama 360 Live
    DEFINE_HOOK(bool, Solis_PhotographyDataUtility_LiveAssetNamesMoveNext, (void* self, void* mtd)) {
        if (!PhotoOwnedEntryEnabled() || !self) {
            return Solis_PhotographyDataUtility_LiveAssetNamesMoveNext_Orig(self, mtd);
        }
        auto data = ReadManagedObjectField(self, "data");
        if (!data) data = ReadManagedObjectField(self, "<>3__data");
        ScopedPhotoTransition scope(data);
        return Solis_PhotographyDataUtility_LiveAssetNamesMoveNext_Orig(self, mtd);
    }

    // Asset download scope: Activity / Contest Activity / Special Activity / Expression
    DEFINE_HOOK(bool, Solis_PhotographyDataUtility_ActivityAssetNamesMoveNext, (void* self, void* mtd)) {
        if (!PhotoOwnedEntryEnabled() || !self) {
            return Solis_PhotographyDataUtility_ActivityAssetNamesMoveNext_Orig(self, mtd);
        }
        auto data = ReadManagedObjectField(self, "data");
        if (!data) data = ReadManagedObjectField(self, "<>3__data");
        ScopedPhotoTransition scope(data);
        return Solis_PhotographyDataUtility_ActivityAssetNamesMoveNext_Orig(self, mtd);
    }

    // 3D live actor creation scope: ALL live photography modes (both preview and real shooting)
    DEFINE_HOOK(void*, Solis_Photography_CreateLiveData, (void* self, void* mtd)) {
        if (!PhotoOwnedEntryEnabled() || !self) {
            return Solis_Photography_CreateLiveData_Orig(self, mtd);
        }
        ScopedPhotoTransition scope(self);
        return Solis_Photography_CreateLiveData_Orig(self, mtd);
    }

    // Lazy LINQ selector in PhotographyLiveTransitionData.CreateLiveData():
    // covers deferred async enumeration in Photography360Live3DPresenter (Panorama 360) and all live modes.
    DEFINE_HOOK(void*, Solis_Photography_CreateLiveDataDescriptor, (void* self, void* actorCostume, void* mtd)) {
        std::string origCharId;
        if (PhotoOwnedEntryEnabled() && actorCostume) {
            if (auto selected = FindPhotoAppearance(actorCostume)) {
                origCharId = GetCostumeCharacterId(GetActorCostumeCostume(actorCostume));
                Log::DebugFmt("PhotoDisplay: CreateLiveData descriptor swapped %p -> %p", actorCostume, selected);
                actorCostume = selected;
            }
        }
        auto descriptor = Solis_Photography_CreateLiveDataDescriptor_Orig(self, actorCostume, mtd);
        if (descriptor && !origCharId.empty() &&
            GetCostumeCharacterId(GetActorCostumeCostume(actorCostume)) != origCharId) {
            SetActorDescriptorNameFromCharacterId(descriptor, origCharId);
        }
        return descriptor;
    }

    // 3D live MV / mode presenter init scope
    DEFINE_HOOK(void, Solis_PhotographyLiveModePresenter_InitializeMoveNext, (void* self, void* mtd)) {
        if (!PhotoOwnedEntryEnabled() || !self || !g_liveModeInitStateMachineClass) {
            Solis_PhotographyLiveModePresenter_InitializeMoveNext_Orig(self, mtd);
            return;
        }
        static const int dataOffset = GetUnboxedStructFieldOffset(g_liveModeInitStateMachineClass, "data");
        auto data = ReadUnboxedStructField<void*>(self, dataOffset);
        ScopedPhotoTransition scope(data);
        Solis_PhotographyLiveModePresenter_InitializeMoveNext_Orig(self, mtd);
    }

    // 3D spot actor creation scope: Activity / Contest Activity / Special Activity / Expression
    DEFINE_HOOK(void, Solis_PhotographySpotData_InitializeMoveNext, (void* self, void* mtd)) {
        if (!PhotoOwnedEntryEnabled() || !self || !g_spotInitStateMachineClass) {
            Solis_PhotographySpotData_InitializeMoveNext_Orig(self, mtd);
            return;
        }
        static const int thisOffset = GetUnboxedStructFieldOffset(g_spotInitStateMachineClass, "<>4__this");
        static const auto getActorCostume = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Photography.Data", "PhotographySpotData", "get_ActorCostume");
        auto spotData = ReadUnboxedStructField<void*>(self, thisOffset);
        auto ownedOutfit = (spotData && getActorCostume)
            ? getActorCostume->Invoke<void*>(spotData, getActorCostume->address) : nullptr;
        ScopedPhotoOwnedOutfit scope(ownedOutfit);
        Solis_PhotographySpotData_InitializeMoveNext_Orig(self, mtd);
    }

    // Central 3D model + asset bundle descriptor substitution for active photography scopes
    DEFINE_HOOK(void*, Solis_SolisActorUtility_CreateActorDescriptor,
        (void* costume, void* hair, void* props, void* mtd)) {
        std::string origCharId;
        if (PhotoOwnedEntryEnabled() && costume && (activePhotoOwnedOutfit || activePhotoTransition)) {
            void* selected = nullptr;
            if (activePhotoOwnedOutfit && GetActorCostumeCostume(activePhotoOwnedOutfit) == costume) {
                selected = FindPhotoAppearance(activePhotoOwnedOutfit);
            }
            if (!selected && activePhotoTransition) {
                selected = ResolveAppearanceFromTransition(activePhotoTransition, costume);
            }
            if (selected) {
                if (auto selectedCostume = GetActorCostumeCostume(selected)) {
                    origCharId = GetCostumeCharacterId(costume);
                    costume = selectedCostume;
                    hair = GetActorCostumeHair(selected);
                    Log::DebugFmt("PhotoDisplay: applied selected appearance costume=%p hair=%p", costume, hair);
                }
            }
        }
        auto descriptor = Solis_SolisActorUtility_CreateActorDescriptor_Orig(costume, hair, props, mtd);
        if (descriptor && !origCharId.empty() && GetCostumeCharacterId(costume) != origCharId) {
            SetActorDescriptorNameFromCharacterId(descriptor, origCharId);
        }
        return descriptor;
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Photo_CheckShootingAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Activity_StartPromotionAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Activity_StartPromotionAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }
    DEFINE_HOOK(void*, Solis_Activity_StartRefreshAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Activity_StartRefreshAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }
    DEFINE_HOOK(void*, Solis_Activity_StartFanEventAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Activity_StartFanEventAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckSpecialShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Photo_CheckSpecialShootingAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CheckExpressionShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Photo_CheckExpressionShootingAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CreateShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Photo_CreateShootingsAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CreateSpecialShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Photo_CreateSpecialShootingsAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Photo_CreateExpressionShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Photo_CreateExpressionShootingsAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoContest_CheckShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_PhotoContest_CheckShootingAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoContest_SubmitShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_PhotoContest_SubmitShootingAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoPanorama_CheckShootingAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_PhotoPanorama_CheckShootingAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_PhotoPanorama_CreateShootingsAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_PhotoPanorama_CreateShootingsAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Costume_SetCostumeAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Costume_SetCostumeAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Costume_SetLiveCostumeAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Costume_SetLiveCostumeAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
    }

    DEFINE_HOOK(void*, Solis_Costume_CheckBulkAsync_Call, (void* self, void* request, void* metadata,
        void* deadline, void* ct, void* mtd)) {
        auto prepared = PhotoRequests::Prepare(request);
        return Solis_Costume_CheckBulkAsync_Call_Orig(self, prepared.get(), metadata, deadline, ct, mtd);
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

    namespace {
        bool ShouldUseCostumeSpineForSdHair(Il2cppString* hairSdHairAssetId) {
            if (!Config::unlockAllLiveCostume || !hairSdHairAssetId) return false;
            const auto assetIdStr = hairSdHairAssetId->ToString();
            if (assetIdStr.empty()) return false;
            if (HasOctoAssetBundle("spi_sd_chr_hair_" + assetIdStr)) return false;
            return HasOctoAssetBundle("spi_sd_chr_cos_" + assetIdStr);
        }
        Il2cppString* ReplaceHairPrefixWithCostume(Il2cppString* origResult) {
            if (!origResult) return origResult;
            auto str = origResult->ToString();
            constexpr std::string_view kHairPrefix = "spi_sd_chr_hair_";
            if (str.rfind(kHairPrefix.data(), 0) == 0) {
                return Il2cppString::New("spi_sd_chr_cos_" + str.substr(kHairPrefix.size()));
            }
            return origResult;
        }
    }

    DEFINE_HOOK(void*, Solis_ActorCostume_get_SdHair, (void* self, void* mtd)) {
        auto ret = Solis_ActorCostume_get_SdHair_Orig(self, mtd);
        if (!Config::unlockAllLiveCostume || !ret) return ret;
        static const auto isOrnamentHair = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Proto.Master", "Hair", "IsOrnamentHair");
        static const auto getBaseHair = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Proto.Master", "Hair", "GetBaseHair");
        static const auto getSdHairAssetId = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                "Solis.Common.Proto.Master", "Hair", "get_SdHairAssetId");
        if (!getSdHairAssetId) return ret;

        void* baseHair = ret;
        if (isOrnamentHair && getBaseHair &&
            isOrnamentHair->Invoke<bool>(ret, isOrnamentHair->address)) {
            baseHair = getBaseHair->Invoke<void*>(ret, getBaseHair->address);
            if (!baseHair) return nullptr;
        }

        auto assetId = getSdHairAssetId->Invoke<Il2cppString*>(baseHair, getSdHairAssetId->address);
        if (!assetId) return nullptr;
        auto assetIdStr = assetId->ToString();
        if (assetIdStr.empty()) return nullptr;

        if (!HasOctoAssetBundle("spi_sd_chr_hair_" + assetIdStr) &&
            !HasOctoAssetBundle("spi_sd_chr_cos_" + assetIdStr)) {
            return nullptr;
        }
        return ret;
    }

    DEFINE_HOOK(Il2cppString*, Solis_AssetNames_Sd_GetCharacterHairSpineTexture, (Il2cppString* hairSdHairAssetId, void* mtd)) {
        auto ret = Solis_AssetNames_Sd_GetCharacterHairSpineTexture_Orig(hairSdHairAssetId, mtd);
        if (ShouldUseCostumeSpineForSdHair(hairSdHairAssetId)) {
            return ReplaceHairPrefixWithCostume(ret);
        }
        return ret;
    }

    DEFINE_HOOK(Il2cppString*, Solis_AssetNames_Sd_GetCharacterHairSpineBaseText, (Il2cppString* hairSdHairAssetId, void* mtd)) {
        auto ret = Solis_AssetNames_Sd_GetCharacterHairSpineBaseText_Orig(hairSdHairAssetId, mtd);
        if (ShouldUseCostumeSpineForSdHair(hairSdHairAssetId)) {
            return ReplaceHairPrefixWithCostume(ret);
        }
        return ret;
    }

    DEFINE_HOOK(Il2cppString*, Solis_AssetNames_Sd_GetCharacterHairSpineTextureText, (Il2cppString* hairSdHairAssetId, void* mtd)) {
        auto ret = Solis_AssetNames_Sd_GetCharacterHairSpineTextureText_Orig(hairSdHairAssetId, mtd);
        if (ShouldUseCostumeSpineForSdHair(hairSdHairAssetId)) {
            return ReplaceHairPrefixWithCostume(ret);
        }
        return ret;
    }

    DEFINE_HOOK(void*, Spine_SkeletonData_FindSkin, (void* self, Il2cppString* skinName, void* mtd)) {
        auto ret = Spine_SkeletonData_FindSkin_Orig(self, skinName, mtd);
        if (ret || !Config::unlockAllLiveCostume || !self || !skinName) return ret;

        const auto nameStr = skinName->ToString();
        const char* baseSkinName = nullptr;
        bool isFront = false;
        if (nameStr == "head_FL_front") {
            baseSkinName = "head_FL";
            isFront = true;
        } else if (nameStr == "head_FL_back") {
            baseSkinName = "head_FL";
            isFront = false;
        } else if (nameStr == "head_FR_front") {
            baseSkinName = "head_FR";
            isFront = true;
        } else if (nameStr == "head_FR_back") {
            baseSkinName = "head_FR";
            isFront = false;
        } else {
            return nullptr;
        }

        auto baseNameObj = Il2cppString::New(baseSkinName);
        if (!baseNameObj) return nullptr;
        void* baseSkin = Spine_SkeletonData_FindSkin_Orig(self, baseNameObj, mtd);
        if (!baseSkin) return nullptr;

        static const auto findSlotIndex = Il2cppUtils::GetMethod("spine-unity.dll",
                "Spine", "SkeletonData", "FindSlotIndex");
        static const auto skinClass = Il2cppUtils::GetClass("spine-unity.dll", "Spine", "Skin");
        static const auto skinCtor = skinClass
                ? Il2cppUtils::il2cpp_class_get_method_from_name(skinClass->address, ".ctor", 1)
                : nullptr;
        static const auto copySkin = skinClass
                ? Il2cppUtils::il2cpp_class_get_method_from_name(skinClass->address, "CopySkin", 1)
                : nullptr;
        static const auto removeAttachment = skinClass
                ? Il2cppUtils::il2cpp_class_get_method_from_name(skinClass->address, "RemoveAttachment", 2)
                : nullptr;

        int32_t splitSlotIndex = -1;
        if (findSlotIndex) {
            if (auto splitDesk = Il2cppString::New("split_desk")) {
                splitSlotIndex = findSlotIndex->Invoke<int32_t>(self, splitDesk, findSlotIndex->address);
            }
            if (splitSlotIndex < 0) {
                if (auto splitBack = Il2cppString::New("split_back")) {
                    splitSlotIndex = findSlotIndex->Invoke<int32_t>(self, splitBack, findSlotIndex->address);
                }
            }
        }

        if (splitSlotIndex < 0 || !skinClass || !skinCtor || !copySkin || !removeAttachment) {
            return isFront ? baseSkin : nullptr;
        }

        void* newSkin = UnityResolve::Invoke<void*>("il2cpp_object_new", skinClass->address);
        if (!newSkin) return isFront ? baseSkin : nullptr;

        reinterpret_cast<void(*)(void*, Il2cppString*, void*)>(skinCtor->methodPointer)(newSkin, skinName, skinCtor);
        reinterpret_cast<void(*)(void*, void*, void*)>(copySkin->methodPointer)(newSkin, baseSkin, copySkin);

        void* orderedDict = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(baseSkin) + 0x18);
        void* keysList = orderedDict ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(orderedDict) + 0x18) : nullptr;
        void* itemsArray = keysList ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(keysList) + 0x10) : nullptr;
        int32_t count = keysList ? *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(keysList) + 0x18) : 0;

        if (itemsArray && count > 0) {
            const auto removeFn = reinterpret_cast<void(*)(void*, int32_t, Il2cppString*, void*)>(removeAttachment->methodPointer);
            const uintptr_t dataBase = reinterpret_cast<uintptr_t>(itemsArray) + 0x20;
            for (int32_t i = 0; i < count; ++i) {
                const uintptr_t entryAddr = dataBase + static_cast<uintptr_t>(i) * 0x20;
                const int32_t slotIndex = *reinterpret_cast<int32_t*>(entryAddr + 0x00);
                auto* entryName = *reinterpret_cast<Il2cppString**>(entryAddr + 0x08);
                const bool entryIsFront = (slotIndex > splitSlotIndex);
                if (entryIsFront != isFront && entryName) {
                    removeFn(newSkin, slotIndex, entryName, removeAttachment);
                }
            }
        }

        static const auto getSkins = Il2cppUtils::GetMethod("spine-unity.dll", "Spine", "SkeletonData", "get_Skins");
        if (getSkins) {
            if (auto skinsList = getSkins->Invoke<void*>(self, getSkins->address)) {
                auto skinsKlass = Il2cppUtils::get_class_from_instance(skinsList);
                auto addMtd = skinsKlass ? Il2cppUtils::il2cpp_class_get_method_from_name(skinsKlass, "Add", 1) : nullptr;
                if (addMtd && addMtd->methodPointer) {
                    reinterpret_cast<void(*)(void*, void*, void*)>(addMtd->methodPointer)(skinsList, newSkin, addMtd);
                }
            }
        }

        return newSkin;
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

    std::atomic<bool> g_userCollectionGetAllListHooked{false};

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
        if (g_userCollectionGetAllListHooked.load(std::memory_order_relaxed)) return origList;
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
        if (!Config::unlockAllLiveCostume || !Config::unlockCrossCharacterHair) return ret;

        static auto UserHairCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.User",
                                                                     "UserHairCollection");
        static auto UserHairCollection_GetAllList_mtd = UserHairCollection_klass ?
                Il2cppUtils::il2cpp_class_get_method_from_name(UserHairCollection_klass->address, "GetAllList", 1) : nullptr;
        static auto UserHairCollection_GetAllList = UserHairCollection_GetAllList_mtd ?
                reinterpret_cast<void* (*)(void*, void*)>(UserHairCollection_GetAllList_mtd->methodPointer) : nullptr;
        if (!UserHairCollection_GetAllList) return ret;

        auto origList = UserHairCollection_GetAllList(self, nullptr);
        if (Solis_UserCostumeCollection_GetAllList_Orig || Solis_UserHairCollection_GetAllList_Orig) return origList;
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

    DEFINE_HOOK(void, Solis_CostumeSheetPresenter_InitializeWithData,
        (void* self, void* characterAndCostumes, void* passFilter, void* costumeSpecifier,
         void* characterPassFilter, bool isAddForceCostume, bool showThumbnailPoseTypeIcon, void* mtd)) {
        const bool allowSdOnly = IsSdScheduleCostumeSpecifier(costumeSpecifier);
        g_activeCostumeSheetAllowsSdOnly.store(allowSdOnly, std::memory_order_relaxed);
        if (Config::unlockAllLiveCostume) {
            auto specKlass = costumeSpecifier ? Il2cppUtils::get_class_from_instance(costumeSpecifier) : nullptr;
            Log::InfoFmt("Costume sheet init: specifier=%s allowSdOnly=%d",
                         (specKlass && specKlass->name) ? specKlass->name : "null",
                         allowSdOnly ? 1 : 0);
        }
        Solis_CostumeSheetPresenter_InitializeWithData_Orig(
                self, characterAndCostumes, passFilter, costumeSpecifier,
                characterPassFilter, isAddForceCostume, showThumbnailPoseTypeIcon, mtd);
    }

    DEFINE_HOOK(void, Solis_CharacterCostumeScreenModel_ctor,
        (void* self, void* param, void* mtd)) {
        Scoped3DCostumeFilter filterGuard(Config::unlockAllLiveCostume);
        Solis_CharacterCostumeScreenModel_ctor_Orig(self, param, mtd);
        if (Config::unlockAllLiveCostume && self) {
            static const auto getUserCharacter = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                    "Solis.OutGame", "CharacterCostumeScreenModel", "get_UserCharacter");
            auto userCharacter = getUserCharacter ? getUserCharacter->Invoke<void*>(self, getUserCharacter->address) : nullptr;
            const auto cid = GetUserCharacterId(userCharacter);
            if (!cid.empty()) {
                PhotoRequests::SetCostumeRoomCharacter(cid);
            }
        }
    }

    void* GetCostumeChangeSheetModel(void* presenter) {
        if (!presenter) return nullptr;
        auto klass = Il2cppUtils::get_class_from_instance(presenter);
        auto mtd = klass ? Il2cppUtils::il2cpp_class_get_method_from_name(klass, "get_Model", 0) : nullptr;
        if (!mtd || !mtd->methodPointer) return nullptr;
        return reinterpret_cast<void* (*)(void*, const Il2cppUtils::MethodInfo*)>(mtd->methodPointer)(presenter, mtd);
    }

    thread_local void* g_preservedCostumeSheetModel = nullptr;
    thread_local void* g_preservedCostumeSheetCharacter = nullptr;

    DEFINE_HOOK(void, Solis_CostumeChangeSheetPresenter_OnSelectCostume,
        (void* self, void* selectCostume, void* mtd)) {
        void* prevModel = g_preservedCostumeSheetModel;
        void* prevCharacter = g_preservedCostumeSheetCharacter;
        if (Config::unlockAllLiveCostume && Config::unlockCrossCharacterCostume && self) {
            auto presenter = ReadManagedObjectField(self, "<>4__this");
            auto model = GetCostumeChangeSheetModel(presenter);
            static const auto getCharacter = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                    "Solis.OutGame", "CostumeChangeSheetModel", "get_Character");
            auto origCharacter = (model && getCharacter)
                ? getCharacter->Invoke<void*>(model, getCharacter->address) : nullptr;
            if (model && origCharacter) {
                g_preservedCostumeSheetModel = model;
                g_preservedCostumeSheetCharacter = origCharacter;
            }
        }
        Solis_CostumeChangeSheetPresenter_OnSelectCostume_Orig(self, selectCostume, mtd);
        g_preservedCostumeSheetModel = prevModel;
        g_preservedCostumeSheetCharacter = prevCharacter;
    }

    DEFINE_HOOK(void, Solis_CostumeChangeSheetPresenter_SetBaseHairItemModels,
        (void* self, void* selectOrnamentHair, void* mtd)) {
        if (Config::unlockAllLiveCostume && Config::unlockCrossCharacterCostume &&
            self && g_preservedCostumeSheetModel && g_preservedCostumeSheetCharacter) {
            auto model = GetCostumeChangeSheetModel(self);
            if (model == g_preservedCostumeSheetModel) {
                auto modelKlass = Il2cppUtils::get_class_from_instance(model);
                auto setChar = modelKlass ? Il2cppUtils::il2cpp_class_get_method_from_name(modelKlass, "set_Character", 1) : nullptr;
                if (setChar && setChar->methodPointer) {
                    reinterpret_cast<void (*)(void*, void*, const Il2cppUtils::MethodInfo*)>(setChar->methodPointer)(
                            model, g_preservedCostumeSheetCharacter, setChar);
                } else if (modelKlass) {
                    auto field = UnityResolve::Invoke<void*>("il2cpp_class_get_field_from_name",
                                                             modelKlass, "<Character>k__BackingField");
                    if (field) {
                        UnityResolve::Invoke<void>("il2cpp_field_set_value", model, field, &g_preservedCostumeSheetCharacter);
                    }
                }
            }
        }
        Solis_CostumeChangeSheetPresenter_SetBaseHairItemModels_Orig(self, selectOrnamentHair, mtd);
    }

    DEFINE_HOOK(void, Solis_CostumeSheetCellModel_set_ActorCostume,
        (void* self, void* value, void* mtd)) {
        if (Config::unlockAllLiveCostume && self && value) {
            static const auto getCharacter = Il2cppUtils::GetMethod("Assembly-CSharp.dll",
                    "Solis.OutGame", "CostumeSheetCellModel", "get_Character");
            auto character = getCharacter ? getCharacter->Invoke<void*>(self, getCharacter->address) : nullptr;
            const auto cid = GetCharacterMasterId(character);
            if (!cid.empty()) {
                RememberActorCostumeOwner(value, cid);
            }
        }
        Solis_CostumeSheetCellModel_set_ActorCostume_Orig(self, value, mtd);
    }

    DEFINE_HOOK(void, Solis_CostumeChangeSheetPresenter_SetCostumeItemModels,
        (void* self, void* passFilter, void* mtd)) {
        // Photo location filters compare costume types directly, without calling
        // IsAppropriateCostume. The original presenter treats a null filter as
        // accepting every costume in this character's collection.
        if (Config::unlockAllLiveCostume && passFilter) {
            passFilter = nullptr;
            Log::Info("Costume UI: location filter cleared for selection list");
        }
        Scoped3DCostumeFilter filterGuard(
                Config::unlockAllLiveCostume && !g_activeCostumeSheetAllowsSdOnly.load(std::memory_order_relaxed));
        Solis_CostumeChangeSheetPresenter_SetCostumeItemModels_Orig(self, passFilter, mtd);
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

    bool needRestoreHides = false;

    DEFINE_HOOK(bool, VLDOF_IsActive, (void* self)) {
        if (Config::enableFreeCamera) return false;
        return VLDOF_IsActive_Orig(self);
    }

    DEFINE_HOOK(void, SolisQualityManager_set_TargetFrameRate, (void* self, float value)) {
        const auto configFps = Config::targetFrameRate;
        SolisQualityManager_set_TargetFrameRate_Orig(self, configFps == 0 ? value : (float)configFps);
    }

    struct GraphicQualityState {
        void* manager = nullptr;
        int gameQuality = 0;
        int appliedOverride = -1;
        bool renderOverride = false;
        float renderScale = 0.0f;
        int maxBufferPixel = 0;
    };

    GraphicQualityState graphicQualityState;
    bool graphicQualityHooksReady = false;

    UnityResolve::Field* GetGraphicQualityField(const char* name) {
        static auto qualityClass = Il2cppUtils::GetClass("solis-submodule.Runtime.dll", "Solis.Common", "SolisQualityManager");
        return qualityClass ? qualityClass->Get<UnityResolve::Field>(name) : nullptr;
    }

    int GetUnityQualityCount() {
        static const int count = [] {
            auto getNames = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "QualitySettings", "get_names");
            if (!getNames) return 0;
            auto names = getNames->Invoke<UnityResolve::UnityType::Array<Il2cppString*>*>();
            return names ? static_cast<int>(names->max_length) : 0;
        }();
        return count;
    }

    void TrackGraphicQualityManager(void* self) {
        if (graphicQualityState.manager == self) return;
        static auto threeD = GetGraphicQualityField("<ThreeD>k__BackingField");
        graphicQualityState = {self, threeD ? Il2cppUtils::ClassGetFieldValue<int>(self, threeD) : 0, -1};
    }

    int GetRequestedGraphicQuality() {
        if (!graphicQualityHooksReady || !Config::useCustomeGraphicSettings) return 0;
        const int quality = NormalizeGraphicQuality(Config::graphicQuality);
        if (quality == 100 && GetUnityQualityCount() == 0) return 0;
        return quality;
    }

    DEFINE_HOOK(void, SolisQualityManager_SetQuality, (void* self, int level, void* method)) {
        TrackGraphicQualityManager(self);
        graphicQualityState.gameQuality = level;
        const int requested = GetRequestedGraphicQuality();
        const int effective = IsGameGraphicQuality(level) && requested != 0 ? requested : level;
        static auto threeD = GetGraphicQualityField("<ThreeD>k__BackingField");
        const bool overrideChanged = requested != 0 || graphicQualityState.appliedOverride > 0;
        if (threeD && overrideChanged && IsGameGraphicQuality(level)) {
            Il2cppUtils::ClassSetFieldValue(self, threeD, 0);
        }
        SolisQualityManager_SetQuality_Orig(self, effective, method);
        if (threeD && overrideChanged) Il2cppUtils::ClassSetFieldValue(self, threeD, level);
        graphicQualityState.appliedOverride = requested;
    }

    DEFINE_HOOK(void, SolisQualityManager_ApplySetting, (void* self, void* method)) {
        if (Config::targetFrameRate != 0) {
            SolisQualityManager_set_TargetFrameRate_Orig(self, Config::targetFrameRate);
        }
        static auto unityLevel = GetGraphicQualityField("QualitySettingsLevel");
        if (unityLevel && GetRequestedGraphicQuality() == 100) {
            const int level = Il2cppUtils::ClassGetFieldValue<int>(self, unityLevel);
            Il2cppUtils::ClassSetFieldValue(self, unityLevel, ClampUnityQuality(level, GetUnityQualityCount()));
        }
        SolisQualityManager_ApplySetting_Orig(self, method);
        if (Config::dbgMode && graphicQualityHooksReady) {
            static auto maxPixels = GetGraphicQualityField("MaxBufferPixel");
            Log::DebugFmt("GraphicQuality: requested=%d game=%d unity=%d max=%d resolutionOverride=%d",
                          GetRequestedGraphicQuality(), graphicQualityState.gameQuality,
                          unityLevel ? Il2cppUtils::ClassGetFieldValue<int>(self, unityLevel) : -1,
                          maxPixels ? Il2cppUtils::ClassGetFieldValue<int>(self, maxPixels) : -1,
                          Config::useCustomeGraphicSettings);
        }
    }

    DEFINE_HOOK(void, SolisQualityManager_Update, (void* self, void* method)) {
        TrackGraphicQualityManager(self);
        const int requested = GetRequestedGraphicQuality();
        const bool renderOverride = Config::useCustomeGraphicSettings;
        const float renderScale = std::isfinite(Config::renderScale) ? std::clamp(Config::renderScale, 0.25f, 2.0f) : 1.0f;
        const int maxPixels = std::clamp(Config::maxBufferPixel, 256, 4096) & ~1;
        const bool resolutionChanged = renderOverride != graphicQualityState.renderOverride ||
                (renderOverride && (renderScale != graphicQualityState.renderScale || maxPixels != graphicQualityState.maxBufferPixel));
        if (graphicQualityHooksReady && (requested != graphicQualityState.appliedOverride || resolutionChanged) &&
                IsGameGraphicQuality(graphicQualityState.gameQuality)) {
            const int effective = requested != 0 ? requested : graphicQualityState.gameQuality;
            static auto threeD = GetGraphicQualityField("<ThreeD>k__BackingField");
            Il2cppUtils::ClassSetFieldValue(self, threeD, 0);
            SolisQualityManager_SetQuality_Orig(self, effective, nullptr);
            Il2cppUtils::ClassSetFieldValue(self, threeD, graphicQualityState.gameQuality);
            graphicQualityState.appliedOverride = requested;
            graphicQualityState.renderOverride = renderOverride;
            graphicQualityState.renderScale = renderScale;
            graphicQualityState.maxBufferPixel = maxPixels;
        }
        SolisQualityManager_Update_Orig(self, method);
    }

    DEFINE_HOOK(void, SolisQualityManager_OnInitialize, (void* self, void* method)) {
        graphicQualityState = {};
        SolisQualityManager_OnInitialize_Orig(self, method);
    }

    DEFINE_HOOK(void, UIManager_UpdateRenderTarget, (UnityResolve::UnityType::Vector2 ratio, void* mtd)) {
        // const auto resolution = GetResolution();
        // Log::DebugFmt("UIManager_UpdateRenderTarget: %f, %f", ratio.x, ratio.y);
        return UIManager_UpdateRenderTarget_Orig(ratio, mtd);
    }

    thread_local void* renderResolutionCamera = nullptr;

    int GetRequestedAntiAliasing() {
        const int mode = Config::antiAliasingMode;
        return Config::useCustomeGraphicSettings && mode >= 0 && mode <= 2 ? mode : -1;
    }

    void* GetMainGraphicCamera() {
        static auto getController = Il2cppUtils::GetMethod("vl-unity.Runtime.dll", "VL", "VLCameraController", "get_Instance");
        static auto controllerClass = Il2cppUtils::GetClass("vl-unity.Runtime.dll", "VL.Rendering", "VLSRPCameraController");
        static auto getCamera = Il2cppUtils::GetMethod("vl-unity.Runtime.dll", "VL.Rendering", "VLSRPCameraController", "get_mainCamera");
        if (!getController || !controllerClass || !getCamera) return nullptr;
        auto controller = getController->Invoke<void*>();
        if (!controller) return nullptr;
        auto actualClass = UnityResolve::Invoke<void*>("il2cpp_object_get_class", controller);
        if (!UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from", controllerClass->address, actualClass)) return nullptr;
        return getCamera->Invoke<void*>(controller);
    }

    int GetUnboxedGraphicsFieldOffset(const char* className, const char* fieldName) {
        auto klass = Il2cppUtils::GetClass("Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", className);
        auto field = klass ? klass->Get<UnityResolve::Field>(fieldName) : nullptr;
        if (!field) return -1;
        uint32_t alignment = 0;
        const int size = UnityResolve::Invoke<int>("il2cpp_class_value_size", klass->address, &alignment);
        const int offset = static_cast<int>(field->offset) - static_cast<int>(sizeof(void*) * 2);
        return offset >= 0 && size >= static_cast<int>(sizeof(void*)) && offset <= size - static_cast<int>(sizeof(void*)) ? offset : -1;
    }

    struct GraphicsCameraData {
        void* camera = nullptr;
        int mode = -2;
        int quality = -2;
    };

    GraphicsCameraData ReadGraphicsCameraData(void* data) {
        static const int cameraOffset = GetUnboxedGraphicsFieldOffset("CameraData", "camera");
        static const int modeOffset = GetUnboxedGraphicsFieldOffset("CameraData", "antialiasing");
        static const int qualityOffset = GetUnboxedGraphicsFieldOffset("CameraData", "antialiasingQuality");
        if (!data || cameraOffset < 0 || modeOffset < 0 || qualityOffset < 0) return {};
        auto bytes = static_cast<const uint8_t*>(data);
        GraphicsCameraData result;
        memcpy(&result.camera, bytes + cameraOffset, sizeof(result.camera));
        memcpy(&result.mode, bytes + modeOffset, sizeof(result.mode));
        memcpy(&result.quality, bytes + qualityOffset, sizeof(result.quality));
        return result;
    }


    DEFINE_HOOK(bool, ActorShadowPass_UpdateShadowData,
                (void* self, void* renderingData, void** settings, int resolution, void* method)) {
        const int requested = Config::useCustomeGraphicSettings ? NormalizeShadowResolution(Config::shadowResolution) : 0;
        if (requested > 0 && renderingData) {
            static const int cameraOffset = GetUnboxedGraphicsFieldOffset("RenderingData", "cameraData");
            static auto getMaxSize = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "SystemInfo", "get_maxTextureSize");
            if (cameraOffset >= 0 && getMaxSize) {
                auto camera = ReadGraphicsCameraData(static_cast<uint8_t*>(renderingData) + cameraOffset).camera;
                if (camera && camera == GetMainGraphicCamera() && requested <= getMaxSize->Invoke<int>()) {
                    resolution = requested;
                }
            }
        }
        return ActorShadowPass_UpdateShadowData_Orig(self, renderingData, settings, resolution, method);
    }

    DEFINE_HOOK(void, UniversalRenderPipeline_InitializeCameraData,
                (void* camera, void* additionalData, bool resolveFinalTarget, void* cameraData, void* method)) {
        const int requested = GetRequestedAntiAliasing();
        if ((!Config::dbgMode && requested < 0) || !camera || !additionalData || camera != GetMainGraphicCamera()) {
            return UniversalRenderPipeline_InitializeCameraData_Orig(camera, additionalData, resolveFinalTarget, cameraData, method);
        }
        static auto cameraClass = Il2cppUtils::GetClass("Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData");
        static auto modeField = cameraClass ? cameraClass->Get<UnityResolve::Field>("m_Antialiasing") : nullptr;
        static auto qualityField = cameraClass ? cameraClass->Get<UnityResolve::Field>("m_AntialiasingQuality") : nullptr;
        static auto postField = cameraClass ? cameraClass->Get<UnityResolve::Field>("m_RenderPostProcessing") : nullptr;
        static auto renderTypeField = cameraClass ? cameraClass->Get<UnityResolve::Field>("m_CameraType") : nullptr;
        if (!modeField || !qualityField || !postField || !renderTypeField ||
                Il2cppUtils::ClassGetFieldValue<int>(additionalData, renderTypeField) != 0) {
            return UniversalRenderPipeline_InitializeCameraData_Orig(camera, additionalData, resolveFinalTarget, cameraData, method);
        }
        const bool postEnabled = Il2cppUtils::ClassGetFieldValue<bool>(additionalData, postField);
        const int originalMode = Il2cppUtils::ClassGetFieldValue<int>(additionalData, modeField);
        const int originalQuality = Il2cppUtils::ClassGetFieldValue<int>(additionalData, qualityField);
        struct CameraAntiAliasingScope {
            void* data;
            UnityResolve::Field* modeField;
            UnityResolve::Field* qualityField;
            int mode;
            int quality;
            ~CameraAntiAliasingScope() {
                Il2cppUtils::ClassSetFieldValue(data, modeField, mode);
                Il2cppUtils::ClassSetFieldValue(data, qualityField, quality);
            }
        } scope{additionalData, modeField, qualityField, originalMode, originalQuality};
        if (requested >= 0 && (postEnabled || requested == 0)) {
            Il2cppUtils::ClassSetFieldValue(additionalData, modeField, requested);
            if (requested == 2) Il2cppUtils::ClassSetFieldValue(additionalData, qualityField, 2);
        }
        UniversalRenderPipeline_InitializeCameraData_Orig(camera, additionalData, resolveFinalTarget, cameraData, method);
        if (Config::dbgMode) {
            static void* lastCamera = nullptr;
            static int lastRequested = -3;
            static int lastMode = -3;
            static bool lastPost = false;
            const auto result = ReadGraphicsCameraData(cameraData);
            if (camera != lastCamera || requested != lastRequested || result.mode != lastMode || postEnabled != lastPost) {
                Log::DebugFmt("AntiAliasing: camera=%p requested=%d original=%d effective=%d quality=%d post=%d cameraDataValid=%d",
                              camera, requested, originalMode, result.mode, result.quality, postEnabled, result.camera == camera);
                lastCamera = camera;
                lastRequested = requested;
                lastMode = result.mode;
                lastPost = postEnabled;
            }
        }
    }

    DEFINE_HOOK(void, VLSRPCameraController_UpdateRenderTarget, (void* self, int width, int height, bool forceAlpha, void* method)) {
        if (!self || (width > 0 && height > 0)) {
            return VLSRPCameraController_UpdateRenderTarget_Orig(self, width, height, forceAlpha, method);
        }
        static auto cameraClass = Il2cppUtils::GetClass("vl-unity.Runtime.dll", "VL.Rendering", "VLSRPCameraController");
        static auto dynamicScaleField = cameraClass ? cameraClass->Get<UnityResolve::Field>("useDynamicScale") : nullptr;
        static auto destroyTarget = cameraClass ? cameraClass->Get<UnityResolve::Method>("DestroyTargetTexture") : nullptr;
        static auto getInstanceId = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine", "Object", "GetInstanceID");
        if (!dynamicScaleField || !destroyTarget || !getInstanceId) {
            return VLSRPCameraController_UpdateRenderTarget_Orig(self, width, height, forceAlpha, method);
        }
        static std::unordered_map<int32_t, bool> resolutionModes;
        const auto instanceId = getInstanceId->Invoke<int32_t>(self);
        const auto previousMode = resolutionModes.find(instanceId);
        const bool rebuild = previousMode == resolutionModes.end()
                ? Config::useCustomeGraphicSettings
                : previousMode->second != Config::useCustomeGraphicSettings;
        if (resolutionModes.size() > 128) resolutionModes.clear();
        resolutionModes[instanceId] = Config::useCustomeGraphicSettings;
        if (rebuild) destroyTarget->Invoke<void>(self);
        if (!Config::useCustomeGraphicSettings) {
            return VLSRPCameraController_UpdateRenderTarget_Orig(self, width, height, forceAlpha, method);
        }
        struct ResolutionScope {
            void* camera;
            void* previousCamera;
            UnityResolve::Field* dynamicScaleField;
            bool dynamicScale;

            ~ResolutionScope() {
                renderResolutionCamera = previousCamera;
                Il2cppUtils::ClassSetFieldValue(camera, dynamicScaleField, dynamicScale);
            }
        } scope{self, renderResolutionCamera, dynamicScaleField,
                Il2cppUtils::ClassGetFieldValue<bool>(self, dynamicScaleField)};
        renderResolutionCamera = self;
        Il2cppUtils::ClassSetFieldValue(self, dynamicScaleField, false);
        VLSRPCameraController_UpdateRenderTarget_Orig(self, width, height, forceAlpha, method);
    }

    DEFINE_HOOK(RenderResolution, VLUtility_GetLimitedResolution, (int32_t screenWidth, int32_t screenHeight,
            UnityResolve::UnityType::Vector2 aspectRatio, int32_t maxBufferPixel, float bufferScale, bool firstCall, void* method)) {
        if (!renderResolutionCamera || !firstCall || !Config::useCustomeGraphicSettings) {
            return VLUtility_GetLimitedResolution_Orig(screenWidth, screenHeight, aspectRatio, maxBufferPixel, bufferScale, firstCall, method);
        }
        const auto base = VLUtility_GetLimitedResolution_Orig(screenWidth, screenHeight, aspectRatio, 0, 1.0f, firstCall, method);
        const auto result = ScaleRenderResolution(base, Config::renderScale, Config::maxBufferPixel);
        if (Config::dbgMode || Config::debugImageResourceLog) {
            Log::DebugFmt("RenderResolution: camera=%p screen=%dx%d base=%dx%d result=%dx%d scale=%.3f max=%d dynamic=off",
                          renderResolutionCamera, screenWidth, screenHeight, base.width, base.height,
                          result.width, result.height, Config::renderScale, Config::maxBufferPixel);
        }
        return result;
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

    float BreastDampingAt60Fps(float size) {
        return std::lerp(0.075f, 0.028f, size - 0.75f);
    }

    struct SwingBonePhysics {
        float damping;
        float stiffness;
        float spring;
        float average;
    };

    SwingBonePhysics GetSwingBonePhysics(float size) {
        const float sizeFactor = size - 0.75f;
        return {
                BreastDampingAt60Fps(size),
                std::lerp(0.075f, 0.035f, sizeFactor),
                std::lerp(0.85f, 1.10f, sizeFactor),
                0.08f
        };
    }

    std::atomic_bool liveSwingTuning{false};

    using BreastTransform = UnityResolve::UnityType::Transform;
    using BreastScaleVector = UnityResolve::UnityType::Vector3;

    struct BreastShapeState {
        BreastScaleVector originalScale;
        BreastScaleVector appliedScale;
        BreastScaleVector originalPosition;
        BreastScaleVector appliedPosition;
        BreastScaleVector depthWeights;
    };

    BreastScaleVector GetBreastDepthWeights(BreastTransform* breast, BreastTransform* end) {
        constexpr float evenWeight = 1.0f / 3.0f;
        if (!end) return {evenWeight, evenWeight, evenWeight};

        // The rig's endpoint defines depth even on meshes with different bone orientations.
        auto endDirection = end->GetPosition() - breast->GetPosition();
        const float squaredLength = endDirection.Length();
        if (!std::isfinite(squaredLength) || squaredLength < 0.000001f) return {evenWeight, evenWeight, evenWeight};
        const auto depth = endDirection / std::sqrt(squaredLength);
        const auto weight = [depth](BreastScaleVector axis) {
            const float alignment = axis.Dot(depth);
            return std::min(alignment * alignment, 1.0f);
        };
        return {weight(breast->GetRight()), weight(breast->GetUp()), weight(breast->GetForward())};
    }

    BreastShapeState* ApplyBreastScale(BreastTransform* breast, BreastTransform* end, float size) {
        // Rig setup runs on the Unity thread. Cache the authored shape for reused bones.
        static std::unordered_map<int32_t, BreastShapeState> shapes;
        if (!breast || (size == 1.0f && shapes.empty())) return nullptr;
        static const auto getInstanceId = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Object", "GetInstanceID");
        if (!getInstanceId) return nullptr;

        using Clock = std::chrono::steady_clock;
        static auto nextPrune = Clock::now();
        const auto now = Clock::now();
        if (now >= nextPrune) {
            static const auto exists = Il2cppUtils::GetMethod(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Object", "DoesObjectWithInstanceIDExist");
            if (exists) {
                std::erase_if(shapes, [](const auto& entry) { return !exists->Invoke<bool>(entry.first); });
            }
            nextPrune = now + std::chrono::seconds(10);
        }

        const auto id = getInstanceId->Invoke<int32_t>(breast);
        const auto currentScale = breast->GetLocalScale();
        const auto currentPosition = breast->GetLocalPosition();
        auto [entry, inserted] = shapes.try_emplace(id);
        auto& state = entry->second;
        if (inserted || currentPosition != state.appliedPosition) {
            state.originalPosition = state.appliedPosition = currentPosition;
        }
        if (inserted || currentScale != state.appliedScale) {
            state.originalScale = state.appliedScale = currentScale;
            state.depthWeights = GetBreastDepthWeights(breast, end);
        }
        const float change = size - 1.0f;
        // Let depth grow with the rest of the shape, with slightly less forward projection.
        const float depth = change > 0.0f ? change * 0.80f : change;
        const auto scale = [change, depth](float weight) {
            return 1.0f + std::lerp(change, depth, weight);
        };
        const BreastScaleVector scaled(state.originalScale.x * scale(state.depthWeights.x),
                                       state.originalScale.y * scale(state.depthWeights.y),
                                       state.originalScale.z * scale(state.depthWeights.z));
        if (scaled == currentScale) return &state;
        breast->SetLocalScale(scaled);
        state.appliedScale = scaled;
#ifndef NDEBUG
        if (Config::dbgMode) {
            Log::DebugFmt("BreastScale: size=%.2f original=(%.2f, %.2f, %.2f) applied=(%.2f, %.2f, %.2f)",
                          size, state.originalScale.x, state.originalScale.y, state.originalScale.z,
                          scaled.x, scaled.y, scaled.z);
        }
#endif
        return &state;
    }

    void ApplyBreastSpacing(BreastTransform* left, BreastShapeState* leftShape,
                            BreastTransform* right, BreastShapeState* rightShape, float size) {
        const float sideFraction = 0.045f * std::clamp((size - 1.30f) / 0.45f, 0.0f, 1.0f);
        BreastScaleVector leftOffset, rightOffset;
        if (sideFraction > 0.0f && leftShape && rightShape) {
            auto* leftParent = left->GetParent();
            auto* rightParent = right->GetParent();
            if (leftParent == rightParent) {
                rightOffset = (BreastScaleVector(rightShape->originalPosition) - leftShape->originalPosition) * sideFraction;
                leftOffset = BreastScaleVector(rightOffset) * -1.0f;
            } else if (leftParent && rightParent) {
                static const auto inverseVector = Il2cppUtils::GetMethod(
                        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform", "InverseTransformVector",
                        {"UnityEngine.Vector3"});
                if (inverseVector) {
                    const auto worldOffset = (rightParent->TransformPoint(rightShape->originalPosition) -
                                              leftParent->TransformPoint(leftShape->originalPosition)) * sideFraction;
                    leftOffset = inverseVector->Invoke<BreastScaleVector>(leftParent, BreastScaleVector(worldOffset) * -1.0f);
                    rightOffset = inverseVector->Invoke<BreastScaleVector>(rightParent, worldOffset);
                }
            }
        }

        const auto move = [](BreastTransform* breast, BreastShapeState* shape, const BreastScaleVector& offset) {
            if (!shape) return false;
            const auto position = BreastScaleVector(shape->originalPosition) + offset;
            if (position == shape->appliedPosition) return false;
            breast->SetLocalPosition(position);
            shape->appliedPosition = position;
            return true;
        };
        const bool movedLeft = move(left, leftShape, leftOffset);
        const bool movedRight = move(right, rightShape, rightOffset);
#ifndef NDEBUG
        if (Config::dbgMode && (movedLeft || movedRight) && leftShape && rightShape) {
            const auto authoredWorldPosition = [](BreastTransform* bone, const BreastShapeState& shape) {
                auto* parent = bone->GetParent();
                return parent ? parent->TransformPoint(shape.originalPosition) : shape.originalPosition;
            };
            const auto originalLeft = authoredWorldPosition(left, *leftShape);
            const auto originalRight = authoredWorldPosition(right, *rightShape);
            const float originalDistance = originalLeft.Distance(originalRight);
            if (originalDistance > 0.0001f) {
                const float distance = left->GetPosition().Distance(right->GetPosition());
                Log::DebugFmt("BreastSpacing: size=%.2f spacing=%.2f%% of original", size, distance * 100.0f / originalDistance);
            }
        }
#endif
    }

    void UpdateSolisSwingBreastBones(void* breastBones) {
        const auto [size, swingLevel] = Config::GetBreastMotion();
        if (!breastBones) return;
        const float scale = size > 0.0f ? size : 1.0f;
        const bool needsSetupTuning = size > 0.0f && swingLevel >= 2 &&
                                      !liveSwingTuning.load(std::memory_order_relaxed);

        static auto ActorSwingBreastBone_klass = Il2cppUtils::GetClass("ActorSwing.Runtime.dll", "ActorSwing",
                                                                       "ActorSwingBreastBone");
        if (!ActorSwingBreastBone_klass) return;

        static auto damping_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("damping");
        static auto stiffness_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("stiffness");
        static auto spring_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("spring");
        static auto average_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("average");
        static auto leftBreast_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("leftBreast");
        static auto rightBreast_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rightBreast");
        static auto leftEnd_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("leftBreastEnd");
        static auto rightEnd_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rightBreastEnd");
        if (!leftBreast_field || !rightBreast_field) {
            return;
        }
        const bool tuneSwing = needsSetupTuning && damping_field && stiffness_field && spring_field && average_field;
        const auto physics = GetSwingBonePhysics(scale);
        Il2cppUtils::Tools::CSListEditor<void*> listEditor(breastBones);
        for (auto bone : listEditor) {
            if (!bone) continue;

            const auto transform = [bone](UnityResolve::Field* field) {
                return field ? Il2cppUtils::ClassGetFieldValue<BreastTransform*>(bone, field) : nullptr;
            };
            auto* left = transform(leftBreast_field);
            auto* right = transform(rightBreast_field);
            auto* leftShape = ApplyBreastScale(left, transform(leftEnd_field), scale);
            auto* rightShape = ApplyBreastScale(right, transform(rightEnd_field), scale);
            ApplyBreastSpacing(left, leftShape, right, rightShape, scale);

            if (tuneSwing) {
                Il2cppUtils::ClassSetFieldValue(bone, damping_field, physics.damping);
                Il2cppUtils::ClassSetFieldValue(bone, stiffness_field, physics.stiffness);
                Il2cppUtils::ClassSetFieldValue(bone, spring_field, physics.spring);
                Il2cppUtils::ClassSetFieldValue(bone, average_field, physics.average);
            }
        }
    }

    void InstallBurstBreastMotion();
    std::atomic_bool burstBreastMotionPending{false};

    DEFINE_HOOK(void*, SolisActorController_InitializeActorSwing_GetBreastBones,
        (void* self, void* modelParts, void* mtd)) {
        if (burstBreastMotionPending.load(std::memory_order_acquire)) InstallBurstBreastMotion();
        auto ret = SolisActorController_InitializeActorSwing_GetBreastBones_Orig(self, modelParts, mtd);
        UpdateSolisSwingBreastBones(ret);
        return ret;
    }

    using SwingRotation = UnityResolve::UnityType::Quaternion;

    struct ActorSwingJobLimitInfo {
        uint8_t useLimit;
        float positive[3];
        float negative[3];
    };

    struct ActorSwingAffineTransform {
        float translation[3];
        SwingRotation rotation;
    };

    static_assert(sizeof(ActorSwingJobLimitInfo) == 0x1c);
    static_assert(offsetof(ActorSwingAffineTransform, rotation) == 0xc);

    constexpr int swingListOffset = 0x48;
    constexpr int swingDrivenOffset = 0xf0;
    constexpr int swingDrivenSize = 0x18;
    constexpr int swingBoneDampingOffset = 0x108;
    constexpr int swingBoneStride = 0x13c;

    struct SwingBoneList {
        uint8_t* data;
        int32_t length;
        int32_t capacity;
    };

    using AnimationStreamDeltaTime = float (*)(const void*, void*);
    AnimationStreamDeltaTime animationStreamDeltaTime = nullptr;

    using SwingVector = UnityResolve::UnityType::Vector3;
    using SwingDrivenIsValid = bool (*)(void*, const void*, void*);
    using SwingDrivenGetGlobalTR = void (*)(void*, const void*, SwingVector*, SwingRotation*, void*);
    SwingDrivenIsValid swingDrivenIsValid = nullptr;
    SwingDrivenGetGlobalTR swingDrivenGetGlobalTR = nullptr;
    using BurstDeltaTime = float (*)(const void*);
    BurstDeltaTime* burstDeltaTime = nullptr;
    using BurstDrivenGetGlobalTR = void (*)(void*, const void*, SwingVector*, SwingRotation*);
    BurstDrivenGetGlobalTR burstDrivenGetGlobalTR = nullptr;

    struct SwingPulse {
        float pitch = 0.0f;
        float roll = 0.0f;
    };

    struct BreastSpring {
        BreastMotion::SpringAxis pitch;
        BreastMotion::SpringAxis roll;
    };

    struct ChestMotionState {
        std::chrono::steady_clock::time_point lastSample{};
        SwingVector position{};
        SwingRotation rotation{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<BreastSpring, 2> springs{};
    };

    struct SwingRecoilFrame {
        std::array<SwingPulse, 2> pulses{};
        unsigned nextSide = 0;
    };

    thread_local SwingRecoilFrame* activeSwingRecoil = nullptr;

    SwingRotation MultiplySwingRotation(const SwingRotation& a, const SwingRotation& b) {
        return {
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        };
    }

    SwingVector ChestRight(const SwingRotation& rotation) {
        return {1.0f - 2.0f * (rotation.y * rotation.y + rotation.z * rotation.z),
                2.0f * (rotation.x * rotation.y + rotation.w * rotation.z),
                2.0f * (rotation.x * rotation.z - rotation.w * rotation.y)};
    }

    std::array<SwingPulse, 2> CalculateChestRecoil(const uint8_t* boneData, const void* stream,
                                                  float dt, float size, bool burst) {
        if (dt < 0.001f || dt > 0.1f) return {};

        alignas(void*) uint8_t driven[swingDrivenSize];
        std::memcpy(driven, boneData + swingDrivenOffset, sizeof(driven));
        SwingVector position;
        SwingRotation rotation;
        if (burst) {
            if (!burstDrivenGetGlobalTR) return {};
            burstDrivenGetGlobalTR(driven, stream, &position, &rotation);
        } else {
            if (!swingDrivenIsValid || !swingDrivenGetGlobalTR ||
                !swingDrivenIsValid(driven, stream, nullptr)) return {};
            swingDrivenGetGlobalTR(driven, stream, &position, &rotation, nullptr);
        }
        const float rotationNorm = rotation.x * rotation.x + rotation.y * rotation.y
                                   + rotation.z * rotation.z + rotation.w * rotation.w;
        if (!std::isfinite(position.Length()) || !(rotationNorm > 0.99f && rotationNorm < 1.01f)) return {};

        using namespace std::chrono_literals;
        static std::mutex stateMutex;
        static std::unordered_map<const uint8_t*, ChestMotionState> states;
        static std::chrono::steady_clock::time_point nextPrune{};
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(stateMutex);
        if (now >= nextPrune) {
            std::erase_if(states, [now](const auto& entry) { return now - entry.second.lastSample > 1s; });
            nextPrune = now + 1s;
        }

        auto& state = states[boneData];
        const SwingVector displacement(position.x - state.position.x, position.y - state.position.y,
                                       position.z - state.position.z);
        const SwingRotation rotationDelta = MultiplySwingRotation(
                {-state.rotation.x, -state.rotation.y, -state.rotation.z, state.rotation.w}, rotation);
        if (now - state.lastSample > 250ms || displacement.Length() > 0.16f || std::abs(rotationDelta.w) < 0.92f) {
            state = {now, position, rotation, {}};
            return {};
        }

        auto right = ChestRight(rotation);
        auto rightDelta = right - ChestRight(state.rotation);
        state.lastSample = now;
        state.position = position;
        state.rotation = rotation;

        const float angularSign = rotationDelta.w >= 0.0f ? 1.0f : -1.0f;
        const float sizeFactor = size - 0.75f;
        const float frequency = std::lerp(19.0f, 13.5f, sizeFactor);
        const float halfWidth = std::lerp(0.085f, 0.125f, sizeFactor);
        const SwingVector forward(2.0f * (rotation.x * rotation.z + rotation.w * rotation.y),
                                  2.0f * (rotation.y * rotation.z - rotation.w * rotation.x),
                                  1.0f - 2.0f * (rotation.x * rotation.x + rotation.y * rotation.y));
        const float pitchLimit = std::lerp(0.26f, 0.42f, sizeFactor);
        const float rollLimit = std::lerp(0.16f, 0.28f, sizeFactor);
        std::array<SwingPulse, 2> pulses;
        for (size_t side = 0; side < pulses.size(); ++side) {
            // Rotation moves the left and right attachment points in opposite directions.
            const float direction = side == 0 ? -1.0f : 1.0f;
            const auto attachmentDelta = rightDelta * (direction * halfWidth);
            const float pitchSpeed = (72.0f * (displacement.y + attachmentDelta.y)
                                      + 5.5f * angularSign * rotationDelta.x) / dt;
            const float rollSpeed = (30.0f * (displacement.Dot(right) + attachmentDelta.Dot(forward))
                                     + 3.5f * angularSign * rotationDelta.z) / dt;
            auto& spring = state.springs[side];
            const float sideFrequency = frequency * (1.0f + direction * 0.06f);
            spring.pitch.Step(pitchSpeed, dt, sideFrequency, pitchLimit);
            spring.roll.Step(rollSpeed, dt, sideFrequency, rollLimit);
            pulses[side] = {spring.pitch.angle, spring.roll.angle};
        }
        return pulses;
    }

    bool HasSwingPhysicsLayout() {
        const auto job = Il2cppUtils::GetClass("ActorSwing.Runtime.dll", "ActorSwing", "ActorSwingJob");
        const auto bone = Il2cppUtils::GetClass("ActorSwing.Runtime.dll", "ActorSwing", "ActorSwingJobBreastBone");
        const auto listField = job ? job->Get<UnityResolve::Field>("breastBones") : nullptr;
        constexpr int objectHeaderSize = 2 * sizeof(void*);
        uint32_t alignment = 0;
        const auto hasPhysicsField = [bone](const char* name, size_t offset) {
            const auto field = bone->Get<UnityResolve::Field>(name);
            return field && field->offset == swingBoneDampingOffset + objectHeaderSize + offset;
        };
        return listField && bone &&
               listField->offset == swingListOffset + objectHeaderSize &&
               UnityResolve::Invoke<int>("il2cpp_class_value_size", bone->address, &alignment) == swingBoneStride &&
               hasPhysicsField("damping", offsetof(SwingBonePhysics, damping)) &&
               hasPhysicsField("stiffness", offsetof(SwingBonePhysics, stiffness)) &&
               hasPhysicsField("spring", offsetof(SwingBonePhysics, spring)) &&
               hasPhysicsField("average", offsetof(SwingBonePhysics, average));
    }

    bool HasSwingDrivenLayout() {
        const auto bone = Il2cppUtils::GetClass("ActorSwing.Runtime.dll", "ActorSwing", "ActorSwingJobBreastBone");
        const auto handle = Il2cppUtils::GetClass("Unity.Animation.Rigging.dll", "UnityEngine.Animations.Rigging",
                                                 "ReadOnlyTransformHandle");
        const auto field = bone ? bone->Get<UnityResolve::Field>("driven") : nullptr;
        uint32_t alignment = 0;
        return field && handle && field->offset == swingDrivenOffset + 2 * sizeof(void*) &&
               UnityResolve::Invoke<int>("il2cpp_class_value_size", handle->address, &alignment) == swingDrivenSize;
    }

    class ScopedBreastMotion {
    public:
        ScopedBreastMotion(const void* job, const void* stream, bool burst) {
            activeSwingRecoil = nullptr;
            const auto [size, swingLevel] = Config::GetBreastMotion();
            if (size <= 0.0f) return;
#ifndef NDEBUG
            static std::atomic_int reportedLevel{-1};
            if (reportedLevel.load(std::memory_order_relaxed) != swingLevel &&
                reportedLevel.exchange(swingLevel, std::memory_order_relaxed) != swingLevel) {
                Log::InfoFmt("ActorSwing: rig intensity %d/5", swingLevel + 1);
            }
#endif
            if (swingLevel < 2) return;
            const float dt = burst
                    ? (burstDeltaTime && *burstDeltaTime ? (*burstDeltaTime)(stream) : 0.0f)
                    : animationStreamDeltaTime(stream, nullptr);
            if (!(dt > 0.0f) || !std::isfinite(dt)) return;

            const void* listAddress;
            std::memcpy(&listAddress, static_cast<const uint8_t*>(job) + swingListOffset, sizeof(listAddress));
            if (!listAddress) return;

            SwingBoneList list;
            std::memcpy(&list, listAddress, sizeof(list));
            if (!list.data || list.length <= 0 || list.length > list.capacity || list.length > maxBones) return;
            bones = list;

            auto physics = GetSwingBonePhysics(size);
            // ActorSwing retains (1 - damping)² of the previous motion per update.
            const float steps = std::min(dt, 0.1f) * 60.0f;
            physics.damping = 1.0f - std::pow(1.0f - physics.damping, steps);
            for (int i = 0; i < bones.length; ++i) {
                auto* parameters = bones.data + i * swingBoneStride + swingBoneDampingOffset;
                std::memcpy(&originalPhysics[i], parameters, sizeof(physics));
                std::memcpy(parameters, &physics, sizeof(physics));
            }
            if (swingLevel >= 3) {
                const float strength = swingLevel == 4 ? 1.65f : 0.5f;
                recoil.pulses = CalculateChestRecoil(bones.data, stream, dt, size, burst);
                for (auto& pulse : recoil.pulses) {
                    pulse.pitch *= strength;
                    pulse.roll *= strength;
                }
                activeSwingRecoil = &recoil;
            }
        }

        ~ScopedBreastMotion() {
            // Keep the rig's authored values so Game default can take effect on the next update.
            for (int i = 0; i < bones.length; ++i) {
                auto* parameters = bones.data + i * swingBoneStride + swingBoneDampingOffset;
                std::memcpy(parameters, &originalPhysics[i], sizeof(SwingBonePhysics));
            }
            activeSwingRecoil = previousRecoil;
        }

    private:
        static constexpr int maxBones = 128;
        SwingRecoilFrame* const previousRecoil = activeSwingRecoil;
        SwingRecoilFrame recoil;
        SwingBoneList bones{};
        SwingBonePhysics originalPhysics[maxBones];
    };

    // On ARM64 the AnimationStream and root transform value types are passed indirectly.
    DEFINE_HOOK(void, ActorSwingJob_ProcessBreastBones,
        (void* self, const void* stream, void* manager, const void* rootTransform, void* method)) {
        const ScopedBreastMotion motion(self, stream, false);
        ActorSwingJob_ProcessBreastBones_Orig(self, stream, manager, rootTransform, method);
    }

    DEFINE_HOOK(void, Burst_ProcessBreastBones,
        (void* self, const void* stream, void* manager, const void* rootTransform)) {
        const ScopedBreastMotion motion(self, stream, true);
        Burst_ProcessBreastBones_Orig(self, stream, manager, rootTransform);
    }

    void ApplyBreastMotion(float weight, const SwingRotation& restRotation, ActorSwingAffineTransform* target) {
        const auto [size, swingLevel] = Config::GetBreastMotion();
        if (size > 0.0f && swingLevel > 0 && weight > 0.0f && target) {
            const auto& swingRotation = target->rotation;
            const float dot = restRotation.x * swingRotation.x + restRotation.y * swingRotation.y
                            + restRotation.z * swingRotation.z + restRotation.w * swingRotation.w;
            // Add some travel to small movements, keeping the rig's large motions and limits.
            const float halfAngle = std::acos(std::min(std::abs(dot), 1.0f));
            const float strength = swingLevel * 0.5f;
            const float maxHalfAngle = std::lerp(0.105f, 0.21f, size - 0.75f)
                                     * (0.5f + 0.5f * strength);
            if (halfAngle > 0.0f && halfAngle < maxHalfAngle) {
                const float gain = 1.0f + std::lerp(0.35f, 1.25f, size - 0.75f) * strength;
                const float multiplier = std::min(gain, maxHalfAngle / halfAngle);
                const float sine = std::sin(halfAngle);
                const float restWeight = std::sin((1.0f - multiplier) * halfAngle) / sine;
                const float swingWeight = std::copysign(std::sin(multiplier * halfAngle) / sine, dot);
                target->rotation = {
                        restRotation.x * restWeight + swingRotation.x * swingWeight,
                        restRotation.y * restWeight + swingRotation.y * swingWeight,
                        restRotation.z * restWeight + swingRotation.z * swingWeight,
                        restRotation.w * restWeight + swingRotation.w * swingWeight
                };
            }
        }
    }

    void ApplyChestRecoil(float weight, ActorSwingAffineTransform* target) {
        if (!activeSwingRecoil) return;
        // ProcessBreastBones calls the layer for the left and right sides in that order.
        const unsigned side = activeSwingRecoil->nextSide;
        activeSwingRecoil->nextSide ^= 1u;
        const auto& pulse = activeSwingRecoil->pulses[side];
        if (!target || !(weight > 0.0f) || (pulse.pitch == 0.0f && pulse.roll == 0.0f)) return;

        // Layer the bounded recoil over the game's limited rotation, respecting the rig's blend weight.
        const float layerWeight = std::min(weight, 1.0f);
        const float pitch = pulse.pitch * layerWeight;
        const float roll = pulse.roll * layerWeight;
        const float sinPitch = std::sin(pitch * 0.5f);
        const float cosPitch = std::cos(pitch * 0.5f);
        const float sinRoll = std::sin(roll * 0.5f);
        const float cosRoll = std::cos(roll * 0.5f);
        target->rotation = MultiplySwingRotation(
                {sinPitch * cosRoll, sinPitch * sinRoll, cosPitch * sinRoll, cosPitch * cosRoll}, target->rotation);
#ifndef NDEBUG
        static std::atomic_flag reported[2]{};
        constexpr float radiansToDegrees = 57.2957795f;
        if (!reported[side].test(std::memory_order_relaxed) &&
            std::max(std::abs(pitch), std::abs(roll)) * radiansToDegrees >= 3.0f &&
            !reported[side].test_and_set(std::memory_order_relaxed)) {
            Log::InfoFmt("ActorSwing: %s recoil applied (pitch %.2f deg, roll %.2f deg)",
                         side == 0 ? "left" : "right", pitch * radiansToDegrees, roll * radiansToDegrees);
        }
#endif
    }

    DEFINE_HOOK(void, ActorSwingJob_ProcessBreastBoneLayer,
        (void* self, float weight, ActorSwingJobLimitInfo limit, SwingRotation restRotation,
         ActorSwingAffineTransform* target, void* method)) {
        ApplyBreastMotion(weight, restRotation, target);
        ActorSwingJob_ProcessBreastBoneLayer_Orig(self, weight, limit, restRotation, target, method);
        ApplyChestRecoil(weight, target);
    }

    // Burst passes its quaternion indirectly and has no trailing IL2CPP MethodInfo argument.
    DEFINE_HOOK(void, Burst_ProcessBreastBoneLayer,
        (void* self, float weight, const ActorSwingJobLimitInfo* limit, const SwingRotation* restRotation,
         ActorSwingAffineTransform* target)) {
        ApplyBreastMotion(weight, *restRotation, target);
        Burst_ProcessBreastBoneLayer_Orig(self, weight, limit, restRotation, target);
        ApplyChestRecoil(weight, target);
    }

    void InstallBurstBreastMotion() {
        static std::mutex installMutex;
        const std::lock_guard lock(installMutex);
        if (!burstBreastMotionPending.load(std::memory_order_relaxed)) return;
        void* burstLibrary = xdl_open("lib_burst_generated.so", XDL_DEFAULT);
        if (!burstLibrary) return;
        burstBreastMotionPending.store(false, std::memory_order_relaxed);

        // The AOT exports use managed signatures as their ELF symbol names.
#define ACTOR_SWING_ASSEMBLY ", ActorSwing.Runtime, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null"
#define SWING_STREAM "UnityEngine.Animations.AnimationStream, UnityEngine.AnimationModule, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null stream"
#define SWING_BURST_SUFFIX ") -> System.Void, mscorlib, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089_cbedf90658c89046353c84cec3f8d441 from ActorSwing.Runtime, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null"
        constexpr char processBones[] =
                "ActorSwing.ActorSwingJob" ACTOR_SWING_ASSEMBLY ".ProcessBreastBones("
                "ActorSwing.ActorSwingJob*" ACTOR_SWING_ASSEMBLY " this, " SWING_STREAM ", "
                "ActorSwing.ActorSwingManagerPropertyData&" ACTOR_SWING_ASSEMBLY " manager, "
                "ActorSwing.ActorSwingAffineTransform" ACTOR_SWING_ASSEMBLY " rootTx" SWING_BURST_SUFFIX;
        constexpr char processLayer[] =
                "ActorSwing.ActorSwingJob" ACTOR_SWING_ASSEMBLY ".ProcessBreastBoneLayer("
                "ActorSwing.ActorSwingJob*" ACTOR_SWING_ASSEMBLY " this, "
                "System.Single, mscorlib, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089 weight, "
                "ActorSwing.ActorSwingJobLimitInfo" ACTOR_SWING_ASSEMBLY " limit, "
                "Unity.Mathematics.quaternion, Unity.Mathematics, Version=1.0.0.0, Culture=neutral, PublicKeyToken=null pDynamicDefaultRotation, "
                "ActorSwing.ActorSwingAffineTransform&" ACTOR_SWING_ASSEMBLY " targetBreastTx" SWING_BURST_SUFFIX;
        constexpr char getGlobalTR[] =
                "UnityEngine.Animations.Rigging.ReadOnlyTransformHandle, Unity.Animation.Rigging, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null.GetGlobalTR("
                "UnityEngine.Animations.Rigging.ReadOnlyTransformHandle*, Unity.Animation.Rigging, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null this, "
                SWING_STREAM ", UnityEngine.Vector3&, UnityEngine.CoreModule, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null position, "
                "UnityEngine.Quaternion&, UnityEngine.CoreModule, Version=0.0.0.0, Culture=neutral, PublicKeyToken=null rotation" SWING_BURST_SUFFIX;
#undef SWING_BURST_SUFFIX
#undef SWING_STREAM
#undef ACTOR_SWING_ASSEMBLY

        burstDeltaTime = static_cast<BurstDeltaTime*>(xdl_sym(
                burstLibrary, "UnityEngine.Animations.AnimationStream::GetDeltaTime_Injected_Ptr", nullptr));
        if (HasSwingDrivenLayout()) {
            burstDrivenGetGlobalTR = reinterpret_cast<BurstDrivenGetGlobalTR>(
                    xdl_sym(burstLibrary, getGlobalTR, nullptr));
        }
        const auto layer = xdl_sym(burstLibrary, processLayer, nullptr);
        const auto bones = xdl_sym(burstLibrary, processBones, nullptr);
        if (layer && bones && burstDeltaTime) {
            const auto installer = Plugin::GetInstance().GetHookInstaller();
            const auto layerHook = installer->InstallHook(layer, reinterpret_cast<void*>(Burst_ProcessBreastBoneLayer_Hook),
                    reinterpret_cast<void**>(&Burst_ProcessBreastBoneLayer_Orig));
            if (layerHook) {
                const auto bonesHook = installer->InstallHook(bones, reinterpret_cast<void*>(Burst_ProcessBreastBones_Hook),
                        reinterpret_cast<void**>(&Burst_ProcessBreastBones_Orig));
                if (bonesHook) {
                    liveSwingTuning.store(true, std::memory_order_relaxed);
                    Log::Info("ActorSwing: Burst breast motion hooks installed");
                }
                else Log::ErrorFmt("ActorSwing: Burst breast bones hook failed (%d)", shadowhook_get_errno());
            }
            else Log::ErrorFmt("ActorSwing: Burst breast layer hook failed (%d)", shadowhook_get_errno());
        }
        else {
            Log::Info("ActorSwing: Burst breast motion exports are unavailable");
        }
        xdl_close(burstLibrary);
    }

    void StartInjectFunctions() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

        UnityResolve::Init(hookInstaller->GetIl2CppLibraryHandle(),
            UnityResolve::Mode::Il2Cpp, Config::lazyInit);
        PrivateServer::Install(hookInstaller);

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

        auto advPresenterClass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.OutGame",
                                                       "ADVEnginePresenter");
        auto advUserNameMethod = advPresenterClass
                ? Il2cppUtils::il2cpp_class_get_method_from_name(advPresenterClass->address,
                                                                 "get_UserName", 0)
                : nullptr;
        if (advUserNameMethod) {
            ADD_HOOK(ADVEnginePresenterBase_get_UserName, advUserNameMethod->methodPointer);
        } else {
            Log::Error("ADVEnginePresenterBase.get_UserName not found");
        }
        ADD_HOOK(MessageDetail_GetReplacedMessage, Il2cppUtils::GetMethodPointer(
                "Assembly-CSharp.dll", "Solis.Common.Proto.Master", "MessageDetail",
                "GetReplacedMessage"));
        ADD_HOOK(MessageDetail_GetNotificationText, Il2cppUtils::GetMethodPointer(
                "Assembly-CSharp.dll", "Solis.Common.Proto.Master", "MessageDetail",
                "GetNotificationText"));

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
        /* SQL query functions, not recommended
        // Example of converting byte[] u8 string to std::string below
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

        ADD_HOOK(Image_set_sprite,
                 Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI",
                                               "Image", "set_sprite", {"UnityEngine.Sprite"}));

        ADD_HOOK(Image_set_overrideSprite,
                 Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI",
                                               "Image", "set_overrideSprite", {"UnityEngine.Sprite"}));

        ADD_HOOK(RawImage_set_texture,
                 Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI",
                                               "RawImage", "set_texture", {"UnityEngine.Texture"}));

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

        // Capture complete protobuf messages before UI collection/availability hooks run.
        for (const auto className : {"UserGetResponse", "UpdatedData", "PhotoListSpecialShootingResponse",
                                    "PromotionProgress", "RefreshProgress", "FanEventProgress"}) {
            auto klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Proto.Api", className);
            if (!klass) continue;
            void* iterator = nullptr;
            while (auto method = UnityResolve::Invoke<Il2cppUtils::MethodInfo*>(
                    "il2cpp_class_get_methods", klass->address, &iterator)) {
                const auto name = UnityResolve::Invoke<const char*>("il2cpp_method_get_name", method);
                const auto count = UnityResolve::Invoke<int>("il2cpp_method_get_param_count", method);
                if (count != 1 || !name || !std::string_view(name).ends_with("InternalMergeFrom")) continue;
                if (std::string_view(className) == "UserGetResponse") {
                    ADD_HOOK(Solis_UserGetResponse_InternalMergeFrom, method->methodPointer);
                } else if (std::string_view(className) == "UpdatedData") {
                    ADD_HOOK(Solis_UpdatedData_InternalMergeFrom, method->methodPointer);
                } else if (std::string_view(className) == "PhotoListSpecialShootingResponse") {
                    ADD_HOOK(Solis_PhotoListSpecialShootingResponse_InternalMergeFrom, method->methodPointer);
                } else if (std::string_view(className) == "PromotionProgress") {
                    ADD_HOOK(Solis_PromotionProgress_InternalMergeFrom, method->methodPointer);
                } else if (std::string_view(className) == "RefreshProgress") {
                    ADD_HOOK(Solis_RefreshProgress_InternalMergeFrom, method->methodPointer);
                } else if (std::string_view(className) == "FanEventProgress") {
                    ADD_HOOK(Solis_FanEventProgress_InternalMergeFrom, method->methodPointer);
                }
                break;
            }
        }
        Log::InfoFmt("ActivityProgress visual hooks ready: promo=%d refresh=%d fanevent=%d",
                     Solis_PromotionProgress_InternalMergeFrom_Orig != nullptr,
                     Solis_RefreshProgress_InternalMergeFrom_Orig != nullptr,
                     Solis_FanEventProgress_InternalMergeFrom_Orig != nullptr);

        uintptr_t sharedUserCollectionGetAllListPtr = 0;
        uintptr_t sharedUserCollectionExistsPtr = 0;
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
                if (Solis_UserCostumeCollection_GetAllList_Orig) {
                    sharedUserCollectionGetAllListPtr = GetAllList_mtd->methodPointer;
                    g_userCollectionGetAllListHooked.store(true, std::memory_order_relaxed);
                }
            }

            auto Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserCostumeCollection_klass->address, "Exists", 1);
            if (Exists_mtd) {
                ADD_HOOK(Solis_UserCostumeCollection_Exists, Exists_mtd->methodPointer);
                if (Solis_UserCostumeCollection_Exists_Orig) {
                    sharedUserCollectionExistsPtr = Exists_mtd->methodPointer;
                }
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
            if (GetAllList_mtd && GetAllList_mtd->methodPointer != sharedUserCollectionGetAllListPtr) {
                ADD_HOOK(Solis_UserHairCollection_GetAllList, GetAllList_mtd->methodPointer);
            }

            auto Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(Solis_UserHairCollection_klass->address, "Exists", 1);
            if (Exists_mtd && Exists_mtd->methodPointer != sharedUserCollectionExistsPtr) {
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
        PhotoRequests::SetValidators(Solis_CostumeSpecifierExtensions_IsAppropriateCostume_Orig,
                                     Solis_Hair_IsWearableCostume_Orig);
        const auto hookNestedMethod = [](const char* ns, const char* outerName, const char* nestedName,
                                         const char* methodName, int argCount, void** outNestedClass = nullptr) -> void* {
            const auto outer = Il2cppUtils::GetClass("Assembly-CSharp.dll", ns, outerName);
            const auto nested = outer ? Il2cppUtils::find_nested_class_from_name(outer->address, nestedName) : nullptr;
            if (outNestedClass) *outNestedClass = nested;
            const auto mtd = nested ? Il2cppUtils::il2cpp_class_get_method_from_name(nested, methodName, argCount) : nullptr;
            return mtd ? reinterpret_cast<void*>(mtd->methodPointer) : nullptr;
        };

        ADD_HOOK(Solis_Photography_ResolveConfirmOutfit,
                 hookNestedMethod("Solis.OutGame", "LivePhotographyScreenPresenter",
                                  "<>c__DisplayClass18_0", "<OnExecuteConfirmSheetAsync>b__0", 1));
        ADD_HOOK(Solis_ActivityPhotography_ResolveConfirmOutfit,
                 hookNestedMethod("Solis.OutGame", "ActivityPhotographyScreenPresenter",
                                  "<>c__DisplayClass9_0", "<OnExecuteConfirmSheetAsync>b__0", 1));
        ADD_HOOK(Solis_SpecialLivePhotography_ResolveConfirmOutfit,
                 hookNestedMethod("Solis.OutGame", "SpecialLivePhotographyScreenPresenter",
                                  "<>c__DisplayClass14_0", "<OnExecuteConfirmSheetAsync>b__0", 1));
        ADD_HOOK(Solis_SpecialActivityPhotography_ResolveConfirmOutfit,
                 hookNestedMethod("Solis.OutGame", "SpecialActivityPhotographyScreenPresenter",
                                  "<>c__DisplayClass11_0", "<OnExecuteConfirmSheetAsync>b__0", 1));
        ADD_HOOK(Solis_PanoramaPhotography_ResolveConfirmOutfit,
                 hookNestedMethod("Solis.OutGame", "PanoramaMusicSelectScreenPresenter",
                                  "<>c__DisplayClass17_0", "<OnExecuteConfirmSheetAsync>b__0", 1));
        ADD_HOOK(Solis_ExpressionPhotography_OnExecuteConfirmMoveNext,
                 hookNestedMethod("Solis.OutGame", "ExpressionPhotographyScreenPresenter",
                                  "<OnExecuteConfirmSheetAsync>d__12", "MoveNext", 0,
                                  &g_expressionConfirmStateMachineClass));
        ADD_HOOK(Solis_PhotoContestLive_CheckShootingMoveNext,
                 hookNestedMethod("Solis.OutGame", "PhotoContestApiUtility",
                                  "<CheckShootingAsync>d__2", "MoveNext", 0,
                                  &g_contestLiveCheckStateMachineClass));
        ADD_HOOK(Solis_PhotoContestActivity_CheckShootingMoveNext,
                 hookNestedMethod("Solis.OutGame", "PhotoContestApiUtility",
                                  "<CheckShootingAsync>d__3", "MoveNext", 0,
                                  &g_contestActivityCheckStateMachineClass));

        ADD_HOOK(Solis_PhotographyDataUtility_LiveAssetNamesMoveNext,
                 hookNestedMethod("Solis.Photography.Data", "PhotographyDataUtility",
                                  "<LiveTransitionDataToAssetNames>d__7", "MoveNext", 0));
        ADD_HOOK(Solis_PhotographyDataUtility_ActivityAssetNamesMoveNext,
                 hookNestedMethod("Solis.Photography.Data", "PhotographyDataUtility",
                                  "<ActivityTransitionDataToAssetNames>d__9", "MoveNext", 0));
        ADD_HOOK(Solis_Photography_CreateLiveData,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Photography.Data",
                                               "PhotographyLiveTransitionData", "CreateLiveData"));
        ADD_HOOK(Solis_Photography_CreateLiveDataDescriptor,
                 hookNestedMethod("Solis.Photography.Data", "PhotographyLiveTransitionData",
                                  "<>c", "<CreateLiveData>b__26_0", 1));
        ADD_HOOK(Solis_PhotographyLiveModePresenter_InitializeMoveNext,
                 hookNestedMethod("Solis.Photography", "PhotographyLiveModePresenter",
                                  "<InitializeAsync>d__16", "MoveNext", 0,
                                  &g_liveModeInitStateMachineClass));
        ADD_HOOK(Solis_PhotographySpotData_InitializeMoveNext,
                 hookNestedMethod("Solis.Photography.Data", "PhotographySpotData",
                                  "<InitializeAsync>d__38", "MoveNext", 0,
                                  &g_spotInitStateMachineClass));
        ADD_HOOK(Solis_SolisActorUtility_CreateActorDescriptor,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.Common",
                                               "SolisActorUtility", "CreateActorDescriptor",
                                               {"Solis.Common.Proto.Master.Costume", "Solis.Common.Proto.Master.Hair", "*"}));
        Log::InfoFmt("PhotoEntry/Display hooks ready: live=%d act=%d expr=%d contest=%d/%d special=%d/%d pano=%d dl=%d/%d render=%d/%d/%d/%d desc=%d",
                     Solis_Photography_ResolveConfirmOutfit_Orig != nullptr,
                     Solis_ActivityPhotography_ResolveConfirmOutfit_Orig != nullptr,
                     Solis_ExpressionPhotography_OnExecuteConfirmMoveNext_Orig != nullptr,
                     Solis_PhotoContestLive_CheckShootingMoveNext_Orig != nullptr,
                     Solis_PhotoContestActivity_CheckShootingMoveNext_Orig != nullptr,
                     Solis_SpecialLivePhotography_ResolveConfirmOutfit_Orig != nullptr,
                     Solis_SpecialActivityPhotography_ResolveConfirmOutfit_Orig != nullptr,
                     Solis_PanoramaPhotography_ResolveConfirmOutfit_Orig != nullptr,
                     Solis_PhotographyDataUtility_LiveAssetNamesMoveNext_Orig != nullptr,
                     Solis_PhotographyDataUtility_ActivityAssetNamesMoveNext_Orig != nullptr,
                     Solis_Photography_CreateLiveData_Orig != nullptr,
                     Solis_Photography_CreateLiveDataDescriptor_Orig != nullptr,
                     Solis_PhotographyLiveModePresenter_InitializeMoveNext_Orig != nullptr,
                     Solis_PhotographySpotData_InitializeMoveNext_Orig != nullptr,
                     Solis_SolisActorUtility_CreateActorDescriptor_Orig != nullptr);
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
        ADD_HOOK(Solis_AssetNames_Sd_GetCharacterHairSpineTexture,
                 hookNestedMethod("Solis.Common.Octo", "AssetNames", "Sd", "GetCharacterHairSpineTexture", 1));
        ADD_HOOK(Solis_AssetNames_Sd_GetCharacterHairSpineBaseText,
                 hookNestedMethod("Solis.Common.Octo", "AssetNames", "Sd", "GetCharacterHairSpineBaseText", 1));
        ADD_HOOK(Solis_AssetNames_Sd_GetCharacterHairSpineTextureText,
                 hookNestedMethod("Solis.Common.Octo", "AssetNames", "Sd", "GetCharacterHairSpineTextureText", 1));
        ADD_HOOK(Spine_SkeletonData_FindSkin,
                 Il2cppUtils::GetMethodPointer("spine-unity.dll", "Spine",
                                               "SkeletonData", "FindSkin"));
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
        ADD_HOOK(Solis_CostumeChangeSheetPresenter_SetCostumeItemModels,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "CostumeChangeSheetPresenter", "SetCostumeItemModels"));
        ADD_HOOK(Solis_CostumeChangeSheetPresenter_OnSelectCostume,
                 hookNestedMethod("Solis.OutGame", "CostumeChangeSheetPresenter",
                                  "<>c__DisplayClass10_0", "<SetEvent>b__0", 1));
        ADD_HOOK(Solis_CostumeChangeSheetPresenter_SetBaseHairItemModels,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "CostumeChangeSheetPresenter", "SetBaseHairItemModels"));
        ADD_HOOK(Solis_CostumeSheetCellModel_set_ActorCostume,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Solis.OutGame",
                                               "CostumeSheetCellModel", "set_ActorCostume"));
        Log::InfoFmt("CrossCostume UI hooks ready: select=%d baseHair=%d cellOwner=%d",
                     Solis_CostumeChangeSheetPresenter_OnSelectCostume_Orig != nullptr,
                     Solis_CostumeChangeSheetPresenter_SetBaseHairItemModels_Orig != nullptr,
                     Solis_CostumeSheetCellModel_set_ActorCostume_Orig != nullptr);
        auto Solis_CostumeSheetPresenter_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.OutGame",
                                                                       "CostumeSheetPresenter");
        if (Solis_CostumeSheetPresenter_klass) {
            auto InitializeWithData_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_CostumeSheetPresenter_klass->address, "InitializeWithData", 6);
            if (InitializeWithData_mtd) {
                ADD_HOOK(Solis_CostumeSheetPresenter_InitializeWithData, InitializeWithData_mtd->methodPointer);
            }
        }
        auto Solis_CharacterCostumeScreenModel_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.OutGame",
                                                                             "CharacterCostumeScreenModel");
        if (Solis_CharacterCostumeScreenModel_klass) {
            auto ctor_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                    Solis_CharacterCostumeScreenModel_klass->address, ".ctor", 1);
            if (ctor_mtd) {
                ADD_HOOK(Solis_CharacterCostumeScreenModel_ctor, ctor_mtd->methodPointer);
            }
        }

        // Resolve nested types by their declaring class rather than a slash-qualified class alias.
        auto apiRoot = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Solis.Common.Network", "Api");
        auto activityType = apiRoot ? Il2cppUtils::find_nested_class_from_name(apiRoot->address, "Activity") : nullptr;
        auto activityCallbacks = activityType ? Il2cppUtils::find_nested_class_from_name(activityType, "<>c") : nullptr;
        if (activityCallbacks) {
            void* iterator = nullptr;
            while (auto method = UnityResolve::Invoke<Il2cppUtils::MethodInfo*>(
                    "il2cpp_class_get_methods", activityCallbacks, &iterator)) {
                const auto name = UnityResolve::Invoke<const char*>("il2cpp_method_get_name", method);
                const auto count = UnityResolve::Invoke<int>("il2cpp_method_get_param_count", method);
                if (!name || count != 4) continue;
                const std::string_view callback(name);
                if (callback.starts_with("<StartPromotionAsync>")) {
                    Log::InfoFmt("Activity hook resolved: %s", name);
                    ADD_HOOK(Solis_Activity_StartPromotionAsync_Call, method->methodPointer);
                } else if (callback.starts_with("<StartRefreshAsync>")) {
                    Log::InfoFmt("Activity hook resolved: %s", name);
                    ADD_HOOK(Solis_Activity_StartRefreshAsync_Call, method->methodPointer);
                } else if (callback.starts_with("<StartFanEventAsync>")) {
                    Log::InfoFmt("Activity hook resolved: %s", name);
                    ADD_HOOK(Solis_Activity_StartFanEventAsync_Call, method->methodPointer);
                }
            }
            Log::InfoFmt("Activity hooks ready: promotion=%d refresh=%d fan-event=%d",
                         Solis_Activity_StartPromotionAsync_Call_Orig != nullptr,
                         Solis_Activity_StartRefreshAsync_Call_Orig != nullptr,
                         Solis_Activity_StartFanEventAsync_Call_Orig != nullptr);
        } else {
            Log::Error("Activity hooks unavailable: Api.Activity.<>c declaring type was not resolved");
        }

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
                                               {"System.Int32", "System.Int32", "System.Boolean"}));

        ADD_HOOK(VLUtility_GetLimitedResolution,
                 Il2cppUtils::GetMethodPointer("vl-unity.Runtime.dll", "VL",
                                               "VLUtility", "GetLimitedResolution",
                                               {"System.Int32", "System.Int32", "UnityEngine.Vector2", "System.Int32", "System.Single", "System.Boolean"}));

        ADD_HOOK(SolisActorController_LateUpdate,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisActorController", "LateUpdate"));

        const auto actorController = Il2cppUtils::GetClass("solis-submodule.Runtime.dll", "Solis.Common",
                                                           "SolisActorController");
        const auto actorSwingCallbacks = actorController
                ? Il2cppUtils::find_nested_class_from_name(actorController->address, "<>c") : nullptr;
        const auto getBreastBones = actorSwingCallbacks
                ? Il2cppUtils::il2cpp_class_get_method_from_name(actorSwingCallbacks,
                                                               "<InitializeActorSwing>b__64_3", 1) : nullptr;
        ADD_HOOK(SolisActorController_InitializeActorSwing_GetBreastBones,
                 getBreastBones ? reinterpret_cast<void*>(getBreastBones->methodPointer) : nullptr);

        ADD_HOOK(ActorSwingJob_ProcessBreastBoneLayer, Il2cppUtils::GetMethodPointer(
                "ActorSwing.Runtime.dll", "ActorSwing", "ActorSwingJob", "ProcessBreastBoneLayer"));

        if (HasSwingPhysicsLayout()) {
            animationStreamDeltaTime = reinterpret_cast<AnimationStreamDeltaTime>(Il2cppUtils::GetMethodPointer(
                    "UnityEngine.AnimationModule.dll", "UnityEngine.Animations", "AnimationStream", "get_deltaTime"));
            if (animationStreamDeltaTime) {
                if (HasSwingDrivenLayout()) {
                    swingDrivenIsValid = reinterpret_cast<SwingDrivenIsValid>(Il2cppUtils::GetMethodPointer(
                            "Unity.Animation.Rigging.dll", "UnityEngine.Animations.Rigging",
                            "ReadOnlyTransformHandle", "IsValid"));
                    swingDrivenGetGlobalTR = reinterpret_cast<SwingDrivenGetGlobalTR>(Il2cppUtils::GetMethodPointer(
                            "Unity.Animation.Rigging.dll", "UnityEngine.Animations.Rigging",
                            "ReadOnlyTransformHandle", "GetGlobalTR"));
                }
                ADD_HOOK(ActorSwingJob_ProcessBreastBones, Il2cppUtils::GetMethodPointer(
                        "ActorSwing.Runtime.dll", "ActorSwing", "ActorSwingJob", "ProcessBreastBones"));
                if (ActorSwingJob_ProcessBreastBones_Orig) liveSwingTuning.store(true, std::memory_order_relaxed);
            }
            // Unity can load Burst after IL2CPP initialization. Retry when the rig is built.
            burstBreastMotionPending.store(true, std::memory_order_release);
            InstallBurstBreastMotion();
        }

        ADD_HOOK(SolisQualityManager_set_TargetFrameRate,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "set_TargetFrameRate"));
        ADD_HOOK(UniversalRenderPipeline_InitializeCameraData,
                 Il2cppUtils::GetMethodPointer("Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal",
                                               "UniversalRenderPipeline", "InitializeCameraData",
                                               {"UnityEngine.Camera", "UnityEngine.Rendering.Universal.UniversalAdditionalCameraData", "System.Boolean", "UnityEngine.Rendering.Universal.CameraData&"}));
        ADD_HOOK(ActorShadowPass_UpdateShadowData, Il2cppUtils::GetMethodPointer("vl-unity-srp.Runtime.dll", "VL.Rendering", "ActorShadowPass", "UpdateShadowData"));
        ADD_HOOK(SolisQualityManager_ApplySetting,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "ApplySetting"));
        ADD_HOOK(SolisQualityManager_SetQuality,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "SetQuality", {"SolisQualityManager.QualityLevel"}));
        ADD_HOOK(SolisQualityManager_Update,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "Update"));
        ADD_HOOK(SolisQualityManager_OnInitialize,
                 Il2cppUtils::GetMethodPointer("solis-submodule.Runtime.dll", "Solis.Common",
                                               "SolisQualityManager", "OnInitialize"));
        graphicQualityHooksReady = SolisQualityManager_SetQuality_Orig && SolisQualityManager_ApplySetting_Orig &&
                SolisQualityManager_Update_Orig && SolisQualityManager_OnInitialize_Orig &&
                GetGraphicQualityField("<ThreeD>k__BackingField") && GetGraphicQualityField("QualitySettingsLevel");

        ADD_HOOK(Internal_LogException, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_LogException(System.Exception,UnityEngine.Object)"));
        ADD_HOOK(Internal_Log, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_Log(UnityEngine.LogType,UnityEngine.LogOption,System.String,UnityEngine.Object)"));

        ADD_HOOK(AudioSource_Play,
                 Il2cppUtils::GetMethodPointer("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "Play"));
        ADD_HOOK(AudioSource_Play_UInt64,
                 Il2cppUtils::GetMethodPointer("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "Play", {"System.UInt64"}));
        ADD_HOOK(AudioSource_PlayDelayed,
                 Il2cppUtils::GetMethodPointer("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "PlayDelayed", {"System.Single"}));
        ADD_HOOK(AudioSource_PlayOneShot,
                 Il2cppUtils::GetMethodPointer("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "PlayOneShot", {"UnityEngine.AudioClip", "System.Single"}));
        ADD_HOOK(AudioSource_set_clip,
                 Il2cppUtils::GetMethodPointer("UnityEngine.AudioModule.dll", "UnityEngine", "AudioSource", "set_clip"));
        // Dual platform
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
    std::string LocalizeNotificationText(std::string text, const std::string& userName) {
        std::string translated;
        if (Local::GetGenericText(text, &translated)) text = std::move(translated);
        return HookMain::ResolveJosa(Config::ReplaceDisplayUserName(std::move(text), userName));
    }

    void Install() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

        Log::Info("Installing hook");

        ADD_HOOK(HookMain::il2cpp_init,
            Plugin::GetInstance().GetHookInstaller()->LookupSymbol("il2cpp_init"));

        Log::Info("Hook installed");
    }
}
