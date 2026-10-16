#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HoshimiLocal {
    struct RenderResolution {
        int32_t width;
        int32_t height;
    };

    inline RenderResolution ScaleRenderResolution(RenderResolution base, float scale, int maxPixels) {
        if (base.width <= 0 || base.height <= 0) return base;
        scale = std::isfinite(scale) ? std::clamp(scale, 0.25f, 2.0f) : 1.0f;
        maxPixels = std::clamp(maxPixels, 256, 4096) & ~1;
        scale = std::min(scale, static_cast<float>(maxPixels) / std::max(base.width, base.height));
        const auto evenDimension = [scale, maxPixels](int dimension) {
            return std::clamp(static_cast<int>(std::round(dimension * scale / 2.0f)) * 2, 16, maxPixels);
        };
        return {evenDimension(base.width), evenDimension(base.height)};
    }
}
