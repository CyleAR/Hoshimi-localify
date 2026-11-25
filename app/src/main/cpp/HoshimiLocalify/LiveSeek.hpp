#pragma once

#include <cstdint>

namespace HoshimiLocal::LiveSeek {
    enum class Mode {
        Live,
        Panorama,
        Activity,
        ActivityTop,
    };

    void BindVisibility(void* owner, void* model);
    void Begin(void* owner, void* player, Mode mode);
    void End(void* owner);
    void Update();
    int64_t Snapshot();
    void Request(int session, float fraction);
    void SetPaused(int session, bool paused);
    void Step(int session, int deltaTicks);
}
