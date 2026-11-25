#include "LiveSeek.hpp"
#include "Il2cppUtils.hpp"
#include "config/Config.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>

namespace HoshimiLocal::LiveSeek {
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr int timeMask = (1 << 20) - 1;
        constexpr int stepBias = 1 << 19;
        constexpr int ticksPerSecond = 100;
        constexpr float stepSeconds = 1.f / ticksPerSecond;
        std::atomic<int64_t> snapshot{0}, request{0};
        std::atomic<int64_t> steps{0};
        std::atomic<int> playbackRequest{0};
        enum class PauseSource { None, Presenter, MovieFrame, Activity };
        PauseSource pauseSource = PauseSource::None;
        int session = 0;
        void* activeOwner = nullptr;
        void* visibilityOwner = nullptr;
        uint32_t playerHandle = 0, visibilityHandle = 0;
        Mode activeMode = Mode::Live;
        void* lastActivityPrimary = nullptr;
        std::vector<uint32_t> pausedActivityHandles;
        Clock::time_point nextUpdate{};
        Clock::time_point nextSeek{};

        void ReplaceWeak(uint32_t& handle, void* target) {
            if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            handle = target ? UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new_weakref", target, false) : 0;
        }

        struct Controls {
            UnityResolve::Method *playing, *duration, *time, *seek, *visible, *paused, *pause, *resume;
        };

        struct ActivityControls {
            UnityResolve::Method *getCurrentSpotData, *getSpotDataArray, *getSpotIsValid, *getActorData, *getActorController;
            UnityResolve::Method *visible;
            UnityResolve::Method *getLength, *getTime, *setTime, *isPause, *pause, *resume;
            Il2cppUtils::MethodInfo *evaluateZero;
        };

        Controls Resolve(bool panoramic) {
            const char* assembly = panoramic ? "solis-submodule.Runtime.dll" : "Assembly-CSharp.dll";
            const char* space = panoramic ? "Solis.Common.Live" : "Solis.Photography";
            const char* type = panoramic ? "SolisLivePlayerBase" : "PhotographyLiveModePresenter";
            auto method = [&](const char* name) { return Il2cppUtils::GetMethod(assembly, space, type, name); };
            return {method("get_IsPlaying"), method("get_Duration"),
                    method(panoramic ? "get_TimelineTime" : "get_CurrentTime"), method("SetTime"),
                    Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography", panoramic ?
                        "Photography360LiveContentModel" : "PhotographyPreviewContentModel", "get_IsUIActive"),
                    method("get_IsPause"),
                    panoramic ? Il2cppUtils::GetMethod(assembly, "Solis.Common.Photography", "Photography360Live3DPresenter", "Pause") : method("Pause"),
                    panoramic ? Il2cppUtils::GetMethod(assembly, "Solis.Common.Photography", "Photography360Live3DPresenter", "Resume") : method("Resume")};
        }

        ActivityControls ResolveActivity() {
            auto animCtrlClass = Il2cppUtils::GetClass("vl-unity.Runtime.dll", "VL", "VLAnimationPlayerController");
            auto animMethod = [&](const char* name) {
                return Il2cppUtils::GetMethod("vl-unity.Runtime.dll", "VL", "VLAnimationPlayerController", name);
            };
            return {
                Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography", "PhotographyActivity3DPresenter", "get_CurrentSpotData"),
                Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography.Data", "PhotographySpotGroupData", "get_SpotDataArray"),
                Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography.Data", "PhotographySpotData", "get_IsValid"),
                Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography.Data", "PhotographySpotData", "get_ActorData"),
                Il2cppUtils::GetMethod("solis-submodule.Runtime.dll", "Solis.Common.Photography", "PhotographyActorData", "get_Controller"),
                Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography", "PhotographyPreviewContentModel", "get_IsUIActive"),
                animMethod("get_length"),
                animMethod("GetTime"),
                animMethod("SetTime"),
                animMethod("IsPause"),
                animMethod("Pause"),
                animMethod("Resume"),
                animCtrlClass ? Il2cppUtils::il2cpp_class_get_method_from_name(animCtrlClass->address, "Evaluate", 0) : nullptr,
            };
        }

        void ResumeSavedActivityControllers(const ActivityControls& act) {
            for (auto handle : pausedActivityHandles) {
                if (!handle) continue;
                auto ctrl = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", handle);
                if (ctrl && act.isPause && act.resume && act.isPause->Invoke<bool>(ctrl, act.isPause->address)) {
                    act.resume->Invoke<void>(ctrl, act.resume->address);
                }
                UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            }
            pausedActivityHandles.clear();
        }

        std::vector<void*> CollectActivityControllers(void* presenter, const ActivityControls& act) {
            std::vector<void*> controllers;
            if (!presenter || !act.getCurrentSpotData || !act.getSpotDataArray || !act.getActorData || !act.getActorController) {
                return controllers;
            }
            auto spotGroup = act.getCurrentSpotData->Invoke<void*>(presenter, act.getCurrentSpotData->address);
            if (!spotGroup) return controllers;
            auto spotArray = act.getSpotDataArray->Invoke<UnityResolve::UnityType::Array<void*>*>(spotGroup, act.getSpotDataArray->address);
            if (!spotArray) return controllers;
            for (auto spot : spotArray->ToVector()) {
                if (!spot) continue;
                if (act.getSpotIsValid && !act.getSpotIsValid->Invoke<bool>(spot, act.getSpotIsValid->address)) continue;
                auto actorData = act.getActorData->Invoke<void*>(spot, act.getActorData->address);
                if (!actorData) continue;
                auto actorCtrl = act.getActorController->Invoke<void*>(actorData, act.getActorController->address);
                if (!actorCtrl) continue;
                auto actorClass = Il2cppUtils::get_class_from_instance(actorCtrl);
                if (!actorClass) continue;
                auto getPlayingMtd = Il2cppUtils::il2cpp_class_get_method_from_name(actorClass, "GetPlayingAnimationController", 0);
                if (!getPlayingMtd) continue;
                void* exc = nullptr;
                auto animCtrl = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", getPlayingMtd, actorCtrl, static_cast<void**>(nullptr), &exc);
                if (exc || !animCtrl) continue;
                if (std::find(controllers.begin(), controllers.end(), animCtrl) == controllers.end()) {
                    controllers.push_back(animCtrl);
                }
            }
            return controllers;
        }

        void* ReadObjectField(void* object, const char* name) {
            if (!object) return nullptr;
            auto klass = Il2cppUtils::get_class_from_instance(object);
            if (!klass) return nullptr;
            auto field = UnityResolve::Invoke<void*>("il2cpp_class_get_field_from_name", klass, name);
            if (!field) return nullptr;
            void* value = nullptr;
            UnityResolve::Invoke<void>("il2cpp_field_get_value", object, field, &value);
            return value;
        }

        bool IsMovieMode(void* player) {
            static const auto isMovie = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Solis.Photography",
                    "PhotographyLiveModePresenter", "get_IsMvMode");
            return isMovie && isMovie->Invoke<bool>(player, isMovie->address);
        }

        bool HoldMovieFrame(void* player, bool hold) {
            static const auto pause = Il2cppUtils::GetMethod("UnityEngine.VideoModule.dll", "UnityEngine.Video", "VideoPlayer", "Pause");
            static const auto play = Il2cppUtils::GetMethod("UnityEngine.VideoModule.dll", "UnityEngine.Video", "VideoPlayer", "Play");
            if (!pause || !play) return false;
            auto presenter = ReadObjectField(player, "_mvPresenter");
            auto video = ReadObjectField(ReadObjectField(presenter, "_videoPlayer"), "_player");
            if (!video) return false;
            const auto action = hold ? pause : play;
            action->Invoke<void>(video, action->address);
            return true;
        }

        void UpdateActivity(void* player, bool enabled, int64_t command, int64_t stepCommand, int playback) {
            static const ActivityControls act = ResolveActivity();
            auto controllers = CollectActivityControllers(player, act);
            if (!enabled) {
                if (pauseSource == PauseSource::Activity) {
                    for (auto ctrl : controllers) {
                        if (act.isPause && act.resume && act.isPause->Invoke<bool>(ctrl, act.isPause->address)) {
                            act.resume->Invoke<void>(ctrl, act.resume->address);
                        }
                    }
                    ResumeSavedActivityControllers(act);
                    pauseSource = PauseSource::None;
                }
                snapshot.store(0);
                return;
            }
            if (!act.getLength || !act.getTime || !act.setTime || controllers.empty()) {
                snapshot.store(0);
                return;
            }
            if (visibilityOwner == activeOwner) {
                auto model = visibilityHandle ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", visibilityHandle) : nullptr;
                if (!model || !act.visible || !act.visible->Invoke<bool>(model, act.visible->address)) {
                    snapshot.store(0);
                    return;
                }
            }
            void* primary = controllers[0];
            if (primary != lastActivityPrimary) {
                if (pauseSource == PauseSource::Activity) {
                    ResumeSavedActivityControllers(act);
                    pauseSource = PauseSource::None;
                }
                lastActivityPrimary = primary;
            }
            const float duration = act.getLength->Invoke<float>(primary, act.getLength->address);
            const float rawTime = act.getTime->Invoke<float>(primary, act.getTime->address);
            if (!std::isfinite(duration) || duration < 0.1f || duration * ticksPerSecond > timeMask || !std::isfinite(rawTime)) {
                snapshot.store(0);
                return;
            }
            float current = std::fmod(rawTime, duration);
            if (current < 0.f) current += duration;

            const bool primaryPaused = act.isPause && act.isPause->Invoke<bool>(primary, act.isPause->address);
            if (!primaryPaused && pauseSource == PauseSource::Activity) {
                ResumeSavedActivityControllers(act);
                pauseSource = PauseSource::None;
            }
            if (playback == session && pauseSource == PauseSource::None && act.pause && act.resume && act.isPause) {
                ResumeSavedActivityControllers(act);
                for (auto ctrl : controllers) {
                    act.pause->Invoke<void>(ctrl, act.pause->address);
                    pausedActivityHandles.push_back(UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new_weakref", ctrl, false));
                }
                pauseSource = PauseSource::Activity;
            } else if (playback == -session && pauseSource == PauseSource::Activity) {
                for (auto ctrl : controllers) {
                    if (act.isPause && act.resume && act.isPause->Invoke<bool>(ctrl, act.isPause->address)) {
                        act.resume->Invoke<void>(ctrl, act.resume->address);
                    }
                }
                ResumeSavedActivityControllers(act);
                pauseSource = PauseSource::None;
            }

            const bool paused = pauseSource != PauseSource::None;
            const bool absoluteSeek = (command >> 20) == session;
            const int stepCount = (stepCommand >> 20) == session ? int(stepCommand & timeMask) - stepBias : 0;
            if (absoluteSeek || stepCount != 0) {
                const float start = absoluteSeek ? (command & timeMask) / float(timeMask) * duration : current;
                if (absoluteSeek) {
                    current = std::clamp(start + stepCount * stepSeconds, 0.f, std::max(0.f, duration - stepSeconds));
                } else {
                    float wrapped = std::fmod(start + stepCount * stepSeconds, duration);
                    if (wrapped < 0.f) wrapped += duration;
                    current = std::clamp(wrapped, 0.f, std::max(0.f, duration - 0.001f));
                }
                for (auto ctrl : controllers) {
                    const float ctrlDuration = act.getLength->Invoke<float>(ctrl, act.getLength->address);
                    float ctrlTime = current;
                    if (std::isfinite(ctrlDuration) && ctrlDuration >= 0.1f && std::abs(ctrlDuration - duration) > 0.01f) {
                        ctrlTime = std::fmod(current, ctrlDuration);
                        if (ctrlTime < 0.f) ctrlTime += ctrlDuration;
                    }
                    act.setTime->Invoke<void>(ctrl, ctrlTime, act.setTime->address);
                    if (act.evaluateZero && act.evaluateZero->methodPointer) {
                        reinterpret_cast<void (*)(void*, void*)>(act.evaluateZero->methodPointer)(ctrl, act.evaluateZero);
                    }
                }
                nextSeek = Clock::now() + std::chrono::milliseconds(33);
            }
            const bool topAnchor = (activeMode == Mode::ActivityTop);
            snapshot.store((int64_t(session) << 42) | (int64_t(topAnchor) << 41) | (int64_t(paused) << 40) |
                           (int64_t(std::lround(duration * ticksPerSecond)) << 20) |
                           int64_t(std::lround(std::clamp(current, 0.f, duration) * ticksPerSecond)));
        }
    }

    void BindVisibility(void* owner, void* model) {
        visibilityOwner = owner;
        ReplaceWeak(visibilityHandle, model);
    }

    void Begin(void* owner, void* player, Mode mode) {
        if (pauseSource == PauseSource::Activity) {
            static const ActivityControls act = ResolveActivity();
            ResumeSavedActivityControllers(act);
        }
        snapshot.store(0);
        request.store(0);
        steps.store(0);
        activeOwner = owner;
        pauseSource = PauseSource::None;
        playbackRequest.store(0);
        activeMode = mode;
        lastActivityPrimary = nullptr;
        session = session % 0x1fffff + 1;
        nextUpdate = {};
        nextSeek = {};
        ReplaceWeak(playerHandle, player);
    }

    void End(void* owner) {
        if (owner == visibilityOwner) BindVisibility(nullptr, nullptr);
        if (owner != activeOwner) return;
        if (pauseSource == PauseSource::Activity) {
            static const ActivityControls act = ResolveActivity();
            ResumeSavedActivityControllers(act);
        }
        activeOwner = nullptr;
        pauseSource = PauseSource::None;
        playbackRequest.store(0);
        lastActivityPrimary = nullptr;
        ReplaceWeak(playerHandle, nullptr);
        snapshot.store(0);
        request.store(0);
        steps.store(0);
    }

    void Update() {
        if (!playerHandle) return;
        const bool enabled = Config::showLiveSeekBar.load();
        if (!enabled && pauseSource == PauseSource::None) {
            snapshot.store(0, std::memory_order_relaxed);
            request.store(0, std::memory_order_relaxed);
            steps.store(0, std::memory_order_relaxed);
            playbackRequest.store(0, std::memory_order_relaxed);
            return;
        }
        const auto now = Clock::now();
        if (now < nextUpdate && (now < nextSeek ||
            (!request.load(std::memory_order_relaxed) && !steps.load(std::memory_order_relaxed)))) return;
        nextUpdate = now + std::chrono::milliseconds(100);
        auto player = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", playerHandle);
        if (!player) { End(activeOwner); return; }
        const auto command = request.exchange(0);
        const auto stepCommand = steps.exchange(0);
        const int playback = playbackRequest.exchange(0);
        if (activeMode == Mode::Activity || activeMode == Mode::ActivityTop) {
            UpdateActivity(player, enabled, command, stepCommand, playback);
            return;
        }
        const bool panorama = (activeMode == Mode::Panorama);
        static const Controls standard = Resolve(false), panoramic = Resolve(true);
        const auto& controls = panorama ? panoramic : standard;
        if (!enabled) {
            if (pauseSource == PauseSource::MovieFrame && controls.playing && controls.playing->Invoke<bool>(player, controls.playing->address))
                HoldMovieFrame(player, false);
            else if (pauseSource == PauseSource::Presenter && controls.paused && controls.resume &&
                     controls.paused->Invoke<bool>(player, controls.paused->address))
                controls.resume->Invoke<void>(player, controls.resume->address);
            pauseSource = PauseSource::None;
            snapshot.store(0);
            return;
        }
        if (!controls.playing || !controls.duration || !controls.time || !controls.seek ||
            (pauseSource != PauseSource::Presenter && !controls.playing->Invoke<bool>(player, controls.playing->address))) {
            pauseSource = PauseSource::None;
            snapshot.store(0);
            return;
        }
        if (visibilityOwner == activeOwner) {
            auto model = visibilityHandle ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", visibilityHandle) : nullptr;
            if (!model || !controls.visible || !controls.visible->Invoke<bool>(model, controls.visible->address)) {
                snapshot.store(0);
                return;
            }
        }
        const float duration = controls.duration->Invoke<float>(player, controls.duration->address);
        float current = controls.time->Invoke<float>(player, controls.time->address);
        if (!std::isfinite(duration) || duration < 0.1f || duration * ticksPerSecond > timeMask || !std::isfinite(current)) {
            snapshot.store(0);
            return;
        }
        const bool gamePaused = controls.paused && controls.paused->Invoke<bool>(player, controls.paused->address);
        if (gamePaused && pauseSource != PauseSource::Presenter) {
            pauseSource = PauseSource::None;
            snapshot.store(0);
            return;
        }
        if (!gamePaused && pauseSource == PauseSource::Presenter) pauseSource = PauseSource::None;
        if (playback == session && pauseSource == PauseSource::None) {
            if (!panorama && IsMovieMode(player)) {
                if (HoldMovieFrame(player, true)) pauseSource = PauseSource::MovieFrame;
            } else if (controls.pause && controls.resume && controls.paused) {
                controls.pause->Invoke<void>(player, controls.pause->address);
                pauseSource = PauseSource::Presenter;
            }
        } else if (playback == -session) {
            if (pauseSource == PauseSource::MovieFrame && HoldMovieFrame(player, false)) {
                pauseSource = PauseSource::None;
            } else if (pauseSource == PauseSource::Presenter && controls.resume) {
                controls.resume->Invoke<void>(player, controls.resume->address);
                pauseSource = PauseSource::None;
            }
        }
        const bool paused = pauseSource != PauseSource::None;
        const bool absoluteSeek = (command >> 20) == session;
        const int stepCount = (stepCommand >> 20) == session ? int(stepCommand & timeMask) - stepBias : 0;
        if (absoluteSeek || stepCount != 0) {
            const float start = absoluteSeek ? (command & timeMask) / float(timeMask) * duration : current;
            current = std::clamp(start + stepCount * stepSeconds, 0.f, duration - stepSeconds);
            controls.seek->Invoke<void>(player, current, controls.seek->address);
            nextSeek = Clock::now() + std::chrono::milliseconds(33);
        }
        snapshot.store((int64_t(session) << 42) | (int64_t(panorama) << 41) | (int64_t(paused) << 40) |
                       (int64_t(std::lround(duration * ticksPerSecond)) << 20) |
                       int64_t(std::lround(std::clamp(current, 0.f, duration) * ticksPerSecond)));
    }

    int64_t Snapshot() { return Config::showLiveSeekBar.load() ? snapshot.load() : 0; }

    void Request(int targetSession, float fraction) {
        if (targetSession <= 0 || targetSession != (Snapshot() >> 42) || !std::isfinite(fraction)) return;
        request.store((int64_t(targetSession) << 20) | int64_t(std::clamp(fraction, 0.f, 1.f) * timeMask));
    }

    void SetPaused(int targetSession, bool paused) {
        if (targetSession > 0 && targetSession == (Snapshot() >> 42))
            playbackRequest.store(paused ? targetSession : -targetSession);
    }

    void Step(int targetSession, int deltaTicks) {
        if (deltaTicks == 0 || deltaTicks < -100 || deltaTicks > 100) return;
        auto pending = steps.load();
        do {
            if (targetSession <= 0 || targetSession != (Snapshot() >> 42)) return;
            const int count = (pending >> 20) == targetSession ? int(pending & timeMask) - stepBias : 0;
            const auto next = (int64_t(targetSession) << 20) |
                    std::clamp(stepBias + count + deltaTicks, 0, timeMask);
            if (steps.compare_exchange_weak(pending, next)) return;
        } while (true);
    }
}
