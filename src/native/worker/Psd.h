#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Image.h"
#include "Reader.h"

// Photoshop PSD/PSB reader. Reads only what it needs: header, image resources, layer records
// (metadata only, never layer pixels) and selected rows of the merged composite image.
// Spec: "Adobe Photoshop File Formats Specification".
namespace dp::psd
{
enum class Mode : uint16_t
{
    Bitmap = 0,
    Grayscale = 1,
    Indexed = 2,
    Rgb = 3,
    Cmyk = 4,
    Multichannel = 7,
    Duotone = 8,
    Lab = 9,
};

struct TopLevelItem
{
    std::wstring name;
    bool isGroup = false;
};

struct Document
{
    bool isPsb = false;
    int channels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int depth = 0;
    Mode mode = Mode::Rgb;
    double dpiX = 0;
    double dpiY = 0;

    std::vector<uint8_t> thumbJpeg;
    bool thumbIsBgr = false;
    int thumbWidth = 0;
    int thumbHeight = 0;

    int hasRealMergedData = -1; // from resource 1057; 0 = saved without "Maximize Compatibility"
    std::vector<uint8_t> palette; // indexed colour: 256 R, 256 G, 256 B

    uint64_t imageDataOffset = 0;

    bool layersParsed = false;
    int layerCount = 0; // layers + groups, without hidden group-end markers
    int groupCount = 0;
    int textLayerCount = 0;
    std::vector<TopLevelItem> topLevel; // top of the Layers panel first
    std::vector<std::wstring> fonts;
};

bool IsPsd(const uint8_t* header, size_t size);

Document ReadDocument(Reader& reader, bool withLayers);

// Samples the merged composite into an image no larger than maxSize (never upscales).
// Returns an empty image when the file has no usable composite.
Image RenderComposite(Reader& reader, const Document& document, int maxSize, int maxSupersample);

const wchar_t* ModeName(Mode mode);
} // namespace dp::psd
