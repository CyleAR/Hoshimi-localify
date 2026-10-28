#include "HoshimiLocalify/Plugin.h"
#include "HoshimiLocalify/Log.h"
#include "HoshimiLocalify/Local.h"
#include "HoshimiLocalify/Hook.h"
#include "HoshimiLocalify/Misc.hpp"

#include <jni.h>
#include <android/log.h>
#include "string"
#include "shadowhook.h"
#include "xdl.h"
#include "HoshimiLocalify/camera/camera.hpp"
#include "HoshimiLocalify/config/Config.hpp"
#include "Joystick/JoystickEvent.h"

JavaVM* g_javaVM = nullptr;
jclass g_hoshimiHookMainClass = nullptr;
jmethodID showToastMethodId = nullptr;

bool UnityResolveProgress::startInit = false;
UnityResolveProgress::Progress UnityResolveProgress::assembliesProgress{};
UnityResolveProgress::Progress UnityResolveProgress::classProgress{};

namespace
{
    class AndroidHookInstaller : public HoshimiLocal::HookInstaller
    {
    public:
        explicit AndroidHookInstaller(const std::string& il2cppLibraryPath, const std::string& localizationFilesDir)
                : m_Il2CppLibrary(xdl_open(il2cppLibraryPath.c_str(), XDL_TRY_FORCE_LOAD))
        {
            this->localizationFilesDir = localizationFilesDir;
        }

        ~AndroidHookInstaller() override {
            if (m_Il2CppLibrary) {
                xdl_close(m_Il2CppLibrary);
            }
        }

        void* InstallHook(void* addr, void* hook, void** orig) override
        {
            return shadowhook_hook_func_addr(addr, hook, orig);
        }

        HoshimiLocal::OpaqueFunctionPointer LookupSymbol(const char* name) override
        {
            return reinterpret_cast<HoshimiLocal::OpaqueFunctionPointer>(xdl_sym(m_Il2CppLibrary, name, NULL));
        }

        void* GetIl2CppLibraryHandle() const override
        {
            return m_Il2CppLibrary;
        }

    private:
        void* m_Il2CppLibrary;
    };
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_javaVM = vm;
    return JNI_VERSION_1_6;
}

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_io_github_cylear_hoshimi_localify_NotificationLocalizer_translateTexts(
        JNIEnv* env, jclass clazz, jstring basePath, jstring userName, jobjectArray texts) {
    if (!basePath || !texts) return texts;
    try {
        auto readString = [env](jstring value) {
            if (!value) return std::string{};
            std::u16string text(env->GetStringLength(value), u'\0');
            env->GetStringRegion(value, 0, static_cast<jsize>(text.size()), reinterpret_cast<jchar*>(text.data()));
            if (env->ExceptionCheck()) return std::string{};
            return HoshimiLocal::Misc::ToUTF8(text);
        };
        const auto path = readString(basePath);
        const auto displayName = readString(userName);
        if (env->ExceptionCheck() || !HoshimiLocal::Local::EnsureNotificationData(path)) return texts;

        const auto length = env->GetArrayLength(texts);
        auto stringClass = env->FindClass("java/lang/String");
        if (!stringClass) return nullptr;
        auto output = env->NewObjectArray(length, stringClass, nullptr);
        env->DeleteLocalRef(stringClass);
        if (!output) return nullptr;
        for (jsize index = 0; index < length; ++index) {
            auto original = static_cast<jstring>(env->GetObjectArrayElement(texts, index));
            if (env->ExceptionCheck()) return nullptr;
            if (!original) continue;
            const auto text = readString(original);
            if (env->ExceptionCheck()) return nullptr;
            const auto localized = HoshimiLocal::Hook::LocalizeNotificationText(text, displayName);
            if (localized == text) {
                env->SetObjectArrayElement(output, index, original);
            } else {
                const auto utf16 = HoshimiLocal::Misc::ToUTF16(localized);
                auto replacement = env->NewString(reinterpret_cast<const jchar*>(utf16.data()), static_cast<jsize>(utf16.size()));
                if (!replacement) return nullptr;
                env->SetObjectArrayElement(output, index, replacement);
                env->DeleteLocalRef(replacement);
            }
            env->DeleteLocalRef(original);
            if (env->ExceptionCheck()) return nullptr;
        }
        return output;
    } catch (const std::exception& exception) {
        HoshimiLocal::Log::ErrorFmt("Notification translation failed: %s", exception.what());
        return texts;
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_cylear_hoshimi_localify_HoshimiHookMain_initHook(JNIEnv *env, jclass clazz, jstring targetLibraryPath,
                                                                 jstring localizationFilesDir) {
    g_hoshimiHookMainClass = clazz;
    showToastMethodId = env->GetStaticMethodID(clazz, "showToast", "(Ljava/lang/String;)V");

    const auto targetLibraryPathChars = env->GetStringUTFChars(targetLibraryPath, nullptr);
    if (!targetLibraryPathChars) return;
    const std::string targetLibraryPathStr = targetLibraryPathChars;
    env->ReleaseStringUTFChars(targetLibraryPath, targetLibraryPathChars);

    const auto localizationFilesDirChars = env->GetStringUTFChars(localizationFilesDir, nullptr);
    if (!localizationFilesDirChars) return;
    const std::string localizationFilesDirCharsStr = localizationFilesDirChars;
    env->ReleaseStringUTFChars(localizationFilesDir, localizationFilesDirChars);

    auto& plugin = HoshimiLocal::Plugin::GetInstance();
    plugin.InstallHook(std::make_unique<AndroidHookInstaller>(targetLibraryPathStr, localizationFilesDirCharsStr));
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_cylear_hoshimi_localify_HoshimiHookMain_keyboardEvent(JNIEnv *env, jclass clazz, jint key_code, jint action) {
    IPCamera::on_cam_rawinput_keyboard(action, key_code);
    const auto msg = HoshimiLocal::Local::OnKeyDown(action, key_code);
    if (!msg.empty()) {
        g_hoshimiHookMainClass = clazz;
        showToastMethodId = env->GetStaticMethodID(clazz, "showToast", "(Ljava/lang/String;)V");

        if (env && clazz && showToastMethodId) {
            jstring param = env->NewStringUTF(msg.c_str());
            env->CallStaticVoidMethod(clazz, showToastMethodId, param);
            env->DeleteLocalRef(param);
        }
    }

}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_cylear_hoshimi_localify_HoshimiHookMain_joystickEvent(JNIEnv *env, jclass clazz,
                                                                      jint action,
                                                                      jfloat leftStickX,
                                                                      jfloat leftStickY,
                                                                      jfloat rightStickX,
                                                                      jfloat rightStickY,
                                                                      jfloat leftTrigger,
                                                                      jfloat rightTrigger,
                                                                      jfloat hatX,
                                                                      jfloat hatY) {
    JoystickEvent event(action, leftStickX, leftStickY, rightStickX, rightStickY, leftTrigger, rightTrigger, hatX, hatY);
    IPCamera::on_cam_rawinput_joystick(event);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_cylear_hoshimi_localify_HoshimiHookMain_loadConfig(JNIEnv *env, jclass clazz,
                                                                   jstring config_json_str) {
    const auto configJsonStrChars = env->GetStringUTFChars(config_json_str, nullptr);
    if (!configJsonStrChars) return;
    const std::string configJson = configJsonStrChars;
    env->ReleaseStringUTFChars(config_json_str, configJsonStrChars);
    HoshimiLocal::Config::LoadConfig(configJson);
}

extern "C"
JNIEXPORT jint JNICALL
Java_io_github_cylear_hoshimi_localify_HoshimiHookMain_pluginCallbackLooper(JNIEnv *env,
                                                                             jclass clazz) {
    // HoshimiLocal::Log::ToastLoop(env, clazz);

    if (UnityResolveProgress::startInit) {
        return 9;
    }
    return 0;
}


extern "C"
JNIEXPORT void JNICALL
Java_io_github_cylear_hoshimi_localify_models_NativeInitProgress_pluginInitProgressLooper(
        JNIEnv *env, jclass clazz, jobject progress) {

    // jclass progressClass = env->GetObjectClass(progress);

    static jfieldID startInitFieldID = env->GetStaticFieldID(clazz, "startInit", "Z");

    static jmethodID setAssembliesProgressDataMethodID = env->GetMethodID(clazz, "setAssembliesProgressData", "(JJ)V");
    static jmethodID setClassProgressDataMethodID = env->GetMethodID(clazz, "setClassProgressData", "(JJ)V");

    // jboolean startInit = env->GetStaticBooleanField(clazz, startInitFieldID);

    env->SetStaticBooleanField(clazz, startInitFieldID, UnityResolveProgress::startInit);

    env->CallVoidMethod(progress, setAssembliesProgressDataMethodID,
                        UnityResolveProgress::assembliesProgress.current, UnityResolveProgress::assembliesProgress.total);
    env->CallVoidMethod(progress, setClassProgressDataMethodID,
                        UnityResolveProgress::classProgress.current, UnityResolveProgress::classProgress.total);

}
