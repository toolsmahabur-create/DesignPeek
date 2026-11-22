#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace dp
{
// Top-down 32-bit BGRA pixels, stride = width * 4.
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;

    bool Empty() const { return width <= 0 || height <= 0; }
};

// Size that fits inside a maxSize x maxSize square, keeping the aspect ratio.
inline std::pair<int, int> FitInside(uint64_t width, uint64_t height, int maxSize, bool allowUpscale)
{
    double scale = std::min(static_cast<double>(maxSize) / static_cast<double>(width),
                            static_cast<double>(maxSize) / static_cast<double>(height));
    if (!allowUpscale)
    {
        scale = std::min(scale, 1.0);
    }
    const int w = std::clamp(static_cast<int>(std::lround(static_cast<double>(width) * scale)), 1, maxSize);
    const int h = std::clamp(static_cast<int>(std::lround(static_cast<double>(height) * scale)), 1, maxSize);
    return {w, h};
}
} // namespace dp
