#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dp
{
// Everything the side panel shows for one file.
struct PreviewData
{
    std::wstring path;
    bool readable = false;

    int imageWidth = 0;
    int imageHeight = 0;
    std::vector<uint8_t> pixels; // top-down BGRA

    std::wstring format;
    uint32_t width = 0;
    uint32_t height = 0;
    double dpi = 0;
    std::wstring mode;
    int depth = 0;
    int layers = -1;
    int groups = 0;
    std::vector<std::pair<bool, std::wstring>> topLevel; // (isGroup, name)
    std::vector<std::wstring> fonts;
    bool noComposite = false;

    uint64_t fileSize = 0;
    FILETIME modified{};
};

bool IsSupportedFile(const std::wstring& path);

// Runs the worker on `path` (in a separate, sandboxed process). Never throws.
std::shared_ptr<PreviewData> RenderPreviewFile(const std::wstring& path, int imageSize);
} // namespace dp
