#include "AppIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dp
{
namespace
{
// Signed distance to a rounded rectangle centred at (cx, cy).
double RoundedBoxDistance(double x, double y, double cx, double cy, double halfW, double halfH, double radius)
{
    const double qx = std::abs(x - cx) - (halfW - radius);
    const double qy = std::abs(y - cy) - (halfH - radius);
    const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    return outside + std::min(std::max(qx, qy), 0.0) - radius;
}

double Coverage(double distance) { return std::clamp(0.5 - distance, 0.0, 1.0); }
} // namespace

HICON CreateAppIcon(int size)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits)
    {
        return nullptr;
    }

    auto* pixels = static_cast<uint32_t*>(bits);
    const double s = size;
    const double c = s / 2.0;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const double px = x + 0.5;
            const double py = y + 0.5;

            // Blue rounded square background, vertical gradient.
            const double outer = Coverage(RoundedBoxDistance(px, py, c, c, s * 0.47, s * 0.47, s * 0.22));
            const double t = py / s;
            double r = 45 + (20 - 45) * t;
            double g = 127 + (90 - 127) * t;
            double b = 249 + (220 - 249) * t;

            // White picture frame.
            const double frameOuter = RoundedBoxDistance(px, py, c, c, s * 0.29, s * 0.23, s * 0.06);
            const double ring = Coverage(std::abs(frameOuter + s * 0.035) - s * 0.035);
            // Sun and mountain inside the frame.
            const double sun = Coverage(std::hypot(px - s * 0.62, py - s * 0.40) - s * 0.06);
            const double mountainTop = s * 0.47;
            const double mountainBase = s * 0.70;
            double mountain = 0;
            if (py >= mountainTop && py <= mountainBase)
            {
                const double half = (py - mountainTop) / (mountainBase - mountainTop) * s * 0.17;
                mountain = Coverage(std::abs(px - s * 0.42) - half);
            }
            const double white = std::clamp(ring + sun + mountain, 0.0, 1.0);
            r += (255 - r) * white;
            g += (255 - g) * white;
            b += (255 - b) * white;

            const uint32_t alpha = static_cast<uint32_t>(std::lround(outer * 255));
            pixels[y * size + x] = (alpha << 24) | (uint32_t(std::lround(r)) << 16) | (uint32_t(std::lround(g)) << 8) |
                                   uint32_t(std::lround(b));
        }
    }

    std::vector<uint8_t> maskBits(static_cast<size_t>((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO icon{TRUE, 0, 0, mask, color};
    HICON result = CreateIconIndirect(&icon);
    DeleteObject(mask);
    DeleteObject(color);
    return result;
}
} // namespace dp
