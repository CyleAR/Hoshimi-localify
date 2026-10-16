#pragma once

#include <algorithm>

namespace HoshimiLocal {
    inline int NormalizeShadowResolution(int resolution) {
        return resolution == 512 || resolution == 1024 || resolution == 2048 || resolution == 4096
               ? resolution : 0;
    }

    inline bool IsGameGraphicQuality(int level) {
        return level == 10 || level == 20 || level == 30 || level == 40 || level == 100;
    }

    inline int NormalizeGraphicQuality(int level) {
        return IsGameGraphicQuality(level) ? level : 0;
    }

    inline int ClampUnityQuality(int level, int count) {
        return count > 0 ? std::clamp(level, 0, count - 1) : 0;
    }
}
