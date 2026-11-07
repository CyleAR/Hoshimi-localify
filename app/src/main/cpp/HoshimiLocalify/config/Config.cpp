#include <string>
#include "Config.hpp"
#include "nlohmann/json.hpp"
#include "../Log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <fstream>

namespace HoshimiLocal::Config {
    bool isConfigInit = false;

    bool dbgMode = false;
    bool enabled = true;
    bool lazyInit = true;
    bool replaceFont = true;
    bool useRuntimeKoreanFont = false;
    bool replaceImages = true;
    bool usePhoneSubtitles = true;
    std::string displayUserName = "";
    bool forceExportResource = false;
    bool textTest = false;
    bool useMasterTrans = true;
    int gameOrientation = 0;
    bool dumpText = false;
    bool debugImageResourceLog = false;
    bool forceImagePreserveAspect = true;
    bool debugMasterDbLog = false;
    bool debugAudioLog = false;
    bool enableFreeCamera = false;
    int targetFrameRate = 0;
    bool unlockAllLive = false;
    bool skipLiveToResult = false;
    bool unlockAllLiveCostume = false;
    bool unlockAllPhotoPose = false;

    bool enableLiveCustomeDress = false;
    std::string liveCustomeHeadId = "";
    std::string liveCustomeCostumeId = "";


    bool useCustomeGraphicSettings = false;
    float renderScale = 0.77f;
    int qualitySettingsLevel = 3;
    int graphicQuality = 0;
    int antiAliasingMode = -1;
    int shadowResolution = 0;
    int volumeIndex = 3;
    int maxBufferPixel = 3384;
    int reflectionQualityLevel = 4;
    int lodQualityLevel = 4;

    static bool enableBreastParam = false;
    static bool bUseScale = false;
    static float bScale = 1.0f;
    static int bSwingIntensity = 2;
    static std::atomic<BreastMotionConfig> activeBreastMotion{{0.0f, 2}};

    bool dmmUnlockSize = false;

    BreastMotionConfig GetBreastMotion() {
        return activeBreastMotion.load(std::memory_order_relaxed);
    }

    std::string ReplaceDisplayUserName(std::string text) {
        return ReplaceDisplayUserName(std::move(text), displayUserName);
    }

    std::string ReplaceDisplayUserName(std::string text, const std::string& userName) {
        if (text.empty() || userName.empty()) return text;

        constexpr const char* placeholder = "{user}";
        constexpr size_t placeholderLength = 6;
        size_t pos = 0;
        while ((pos = text.find(placeholder, pos)) != std::string::npos) {
            text.replace(pos, placeholderLength, userName);
            pos += userName.length();
        }
        return text;
    }

    void LoadConfig(const std::string& configStr) {
        try {
            const auto config = nlohmann::json::parse(configStr);

            #define GetConfigItem(name) if (config.contains(#name)) name = config[#name]

            GetConfigItem(dbgMode);
            GetConfigItem(enabled);
            GetConfigItem(lazyInit);
            GetConfigItem(replaceFont);
            GetConfigItem(useRuntimeKoreanFont);
            GetConfigItem(replaceImages);
            GetConfigItem(usePhoneSubtitles);
            GetConfigItem(displayUserName);
            GetConfigItem(forceExportResource);
            GetConfigItem(gameOrientation);
            GetConfigItem(textTest);
            GetConfigItem(useMasterTrans);
            GetConfigItem(dumpText);
            GetConfigItem(debugImageResourceLog);
            GetConfigItem(forceImagePreserveAspect);
            GetConfigItem(debugMasterDbLog);
            GetConfigItem(debugAudioLog);
            GetConfigItem(targetFrameRate);
            GetConfigItem(enableFreeCamera);
            GetConfigItem(unlockAllLive);
            GetConfigItem(skipLiveToResult);
            GetConfigItem(unlockAllLiveCostume);
            GetConfigItem(unlockAllPhotoPose);
            GetConfigItem(enableLiveCustomeDress);
            GetConfigItem(liveCustomeHeadId);
            GetConfigItem(liveCustomeCostumeId);
            GetConfigItem(useCustomeGraphicSettings);
            GetConfigItem(renderScale);
            GetConfigItem(qualitySettingsLevel);
            graphicQuality = config.value("graphicQuality", 0);
            antiAliasingMode = config.value("antiAliasingMode", -1);
            shadowResolution = config.value("shadowResolution", 0);
            GetConfigItem(volumeIndex);
            GetConfigItem(maxBufferPixel);
            GetConfigItem(reflectionQualityLevel);
            GetConfigItem(lodQualityLevel);
            GetConfigItem(enableBreastParam);
            GetConfigItem(bUseScale);
            GetConfigItem(bScale);
            const auto swingIntensity = config.find("bSwingIntensity");
            bSwingIntensity = swingIntensity != config.end() && swingIntensity->is_number_integer()
                    ? std::clamp(swingIntensity->get<int>(), 0, 4)
                    : std::clamp(config.value("bSwingLevel", 1), 0, 2) * 2;
            GetConfigItem(dmmUnlockSize);
            const float size = bUseScale && std::isfinite(bScale)
                    ? std::clamp(bScale, 0.75f, 1.75f) : 1.0f;
            activeBreastMotion.store({enabled && enableBreastParam ? size : 0.0f, bSwingIntensity},
                                     std::memory_order_relaxed);
        }
        catch (std::exception& e) {
            Log::ErrorFmt("LoadConfig error: %s", e.what());
        }
        isConfigInit = true;
    }

    void SaveConfig(const std::string& configPath) {
        try {
            nlohmann::json config;

            #define SetConfigItem(name) config[#name] = name

            SetConfigItem(dbgMode);
            SetConfigItem(enabled);
            SetConfigItem(lazyInit);
            SetConfigItem(replaceFont);
            SetConfigItem(useRuntimeKoreanFont);
            SetConfigItem(replaceImages);
            SetConfigItem(usePhoneSubtitles);
            SetConfigItem(displayUserName);
            SetConfigItem(forceExportResource);
            SetConfigItem(gameOrientation);
            SetConfigItem(textTest);
            SetConfigItem(useMasterTrans);
            SetConfigItem(dumpText);
            SetConfigItem(debugImageResourceLog);
            SetConfigItem(forceImagePreserveAspect);
            SetConfigItem(debugMasterDbLog);
            SetConfigItem(debugAudioLog);
            SetConfigItem(targetFrameRate);
            SetConfigItem(enableFreeCamera);
            SetConfigItem(unlockAllLive);
            SetConfigItem(skipLiveToResult);
            SetConfigItem(unlockAllLiveCostume);
            SetConfigItem(unlockAllPhotoPose);
            SetConfigItem(enableLiveCustomeDress);
            SetConfigItem(liveCustomeHeadId);
            SetConfigItem(liveCustomeCostumeId);
            SetConfigItem(useCustomeGraphicSettings);
            SetConfigItem(renderScale);
            SetConfigItem(qualitySettingsLevel);
            SetConfigItem(graphicQuality);
            SetConfigItem(antiAliasingMode);
            SetConfigItem(shadowResolution);
            SetConfigItem(volumeIndex);
            SetConfigItem(maxBufferPixel);
            SetConfigItem(reflectionQualityLevel);
            SetConfigItem(lodQualityLevel);
            SetConfigItem(enableBreastParam);
            SetConfigItem(bUseScale);
            SetConfigItem(bScale);
            SetConfigItem(bSwingIntensity);
            SetConfigItem(dmmUnlockSize);

            std::ofstream out(configPath);
            if (!out) {
                Log::ErrorFmt("SaveConfig error: Cannot open file: %s", configPath.c_str());
                return;
            }
            out << config.dump(4);
			Log::Info("SaveConfig success");
        }
        catch (std::exception& e) {
            Log::ErrorFmt("SaveConfig error: %s", e.what());
        }
    }
}
