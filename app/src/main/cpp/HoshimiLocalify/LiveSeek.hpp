#pragma once

#include <cstdint>

namespace HoshimiLocal::LiveSeek {
    void BindVisibility(void* owner, void* model);
    void Begin(void* owner, void* player, bool panorama);
    void End(void* owner);
    void Update();
    int64_t Snapshot();
    void Request(int session, float fraction);
    void SetPaused(int session, bool paused);
    void Step(int session, int direction);
}
