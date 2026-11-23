#include "LiveSeek.hpp"
#include "Il2cppUtils.hpp"
#include "config/Config.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace HoshimiLocal::LiveSeek {
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr int timeMask = (1 << 20) - 1;
        constexpr int stepBias = 1 << 19;
        constexpr int ticksPerSecond = 20;
        constexpr float stepSeconds = 1.f / ticksPerSecond;
        std::atomic<int64_t> snapshot{0}, request{0};
        std::atomic<int64_t> steps{0};
        std::atomic<int> playbackRequest{0};
        enum class PauseSource { None, Presenter, MovieFrame };
        PauseSource pauseSource = PauseSource::None;
        int session = 0;
        void* activeOwner = nullptr;
        void* visibilityOwner = nullptr;
        uint32_t playerHandle = 0, visibilityHandle = 0;
        bool panorama = false;
        Clock::time_point nextUpdate{};
        Clock::time_point nextSeek{};

        void ReplaceWeak(uint32_t& handle, void* target) {
            if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
            handle = target ? UnityResolve::Invoke<uint32_t>("il2cpp_gchandle_new_weakref", target, false) : 0;
        }

        struct Controls {
            UnityResolve::Method *playing, *duration, *time, *seek, *visible, *paused, *pause, *resume;
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
    }

    void BindVisibility(void* owner, void* model) {
        visibilityOwner = owner;
        ReplaceWeak(visibilityHandle, model);
    }

    void Begin(void* owner, void* player, bool panoramic) {
        snapshot.store(0);
        request.store(0);
        steps.store(0);
        activeOwner = owner;
        pauseSource = PauseSource::None;
        playbackRequest.store(0);
        panorama = panoramic;
        session = session % 0x1fffff + 1;
        nextUpdate = {};
        nextSeek = {};
        ReplaceWeak(playerHandle, player);
    }

    void End(void* owner) {
        if (owner == visibilityOwner) BindVisibility(nullptr, nullptr);
        if (owner != activeOwner) return;
        activeOwner = nullptr;
        pauseSource = PauseSource::None;
        playbackRequest.store(0);
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
        static const Controls standard = Resolve(false), panoramic = Resolve(true);
        const auto& controls = panorama ? panoramic : standard;
        const auto command = request.exchange(0);
        const auto stepCommand = steps.exchange(0);
        const int playback = playbackRequest.exchange(0);
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

    void Step(int targetSession, int direction) {
        if (direction != -1 && direction != 1) return;
        auto pending = steps.load();
        do {
            if (targetSession <= 0 || targetSession != (Snapshot() >> 42)) return;
            const int count = (pending >> 20) == targetSession ? int(pending & timeMask) - stepBias : 0;
            const auto next = (int64_t(targetSession) << 20) |
                    std::clamp(stepBias + count + direction, 0, timeMask);
            if (steps.compare_exchange_weak(pending, next)) return;
        } while (true);
    }
}
