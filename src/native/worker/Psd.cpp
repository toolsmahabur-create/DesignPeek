#include "Psd.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>

namespace dp::psd
{
namespace
{
constexpr uint32_t Tag(const char (&s)[5])
{
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) |
           uint32_t(uint8_t(s[3]));
}

constexpr uint64_t kMaxThumbnailBytes = 8 * 1024 * 1024;
constexpr uint64_t kMaxTextBlockBytes = 8 * 1024 * 1024;
constexpr int kMaxLayers = 20000;
constexpr size_t kMaxTopLevel = 200;
constexpr size_t kMaxFonts = 200;

bool IsResourceSignature(uint32_t s)
{
    return s == Tag("8BIM") || s == Tag("MeSa") || s == Tag("AgHg") || s == Tag("PHUT") || s == Tag("DCSR");
}

bool IsBlockSignature(uint32_t s) { return s == Tag("8BIM") || s == Tag("8B64"); }

// Keys whose length field is 8 bytes in PSB files.
bool IsLongKey(uint32_t key)
{
    static constexpr uint32_t keys[] = {Tag("LMsk"), Tag("Lr16"), Tag("Lr32"), Tag("Layr"), Tag("Mt16"),
                                        Tag("Mt32"), Tag("Mtrn"), Tag("Alph"), Tag("FMsk"), Tag("lnk2"),
                                        Tag("FEid"), Tag("FXid"), Tag("PxSD")};
    return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
}

std::wstring FromAnsi(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
    {
        return {};
    }
    const int count = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                        text.data(), count);
    return text;
}

std::wstring FromUtf16BE(const uint8_t* data, size_t bytes)
{
    std::wstring text;
    text.reserve(bytes / 2);
    for (size_t i = 0; i + 1 < bytes; i += 2)
    {
        const wchar_t c = static_cast<wchar_t>((data[i] << 8) | data[i + 1]);
        if (c == 0)
        {
            break;
        }
        text.push_back(c);
    }
    return text;
}

double ToDpi(double value, uint16_t unit) { return value <= 0 ? 0 : (unit == 2 ? value * 2.54 : value); }

void ReadThumbnail(Reader& r, uint32_t size, bool isBgr, Document& doc)
{
    if (size <= 28)
    {
        return;
    }
    const uint32_t format = r.U32();
    const int32_t width = r.I32();
    const int32_t height = r.I32();
    r.Skip(8); // width bytes, total size
    const uint32_t compressedSize = r.U32();
    r.Skip(4); // bits per pixel, planes
    if (format != 1 || width <= 0 || height <= 0)
    {
        return;
    }
    const uint64_t length = std::min<uint64_t>(compressedSize, size - 28);
    if (length == 0 || length > kMaxThumbnailBytes)
    {
        return;
    }
    doc.thumbJpeg = r.Bytes(length);
    doc.thumbIsBgr = isBgr;
    doc.thumbWidth = width;
    doc.thumbHeight = height;
}

void ReadResources(Reader& r, uint64_t start, uint64_t end, Document& doc)
{
    uint64_t position = start;
    while (position + 12 <= end)
    {
        r.Seek(position);
        if (!IsResourceSignature(r.U32()))
        {
            break;
        }
        const uint16_t id = r.U16();
        const uint64_t nameLength = r.U8();
        r.Skip(((nameLength + 2) & ~uint64_t(1)) - 1); // Pascal string padded to even
        const uint32_t size = r.U32();
        const uint64_t dataEnd = r.Pos() + size;
        if (dataEnd > end)
        {
            break;
        }

        switch (id)
        {
        case 1036: // thumbnail (RGB JPEG)
            ReadThumbnail(r, size, false, doc);
            break;
        case 1033: // Photoshop 4 thumbnail (BGR JPEG)
            if (doc.thumbJpeg.empty())
            {
                ReadThumbnail(r, size, true, doc);
            }
            break;
        case 1005: // ResolutionInfo
            if (size >= 16)
            {
                const double h = r.U32() / 65536.0;
                const uint16_t hUnit = r.U16();
                r.Skip(2);
                const double v = r.U32() / 65536.0;
                const uint16_t vUnit = r.U16();
                doc.dpiX = ToDpi(h, hUnit);
                doc.dpiY = ToDpi(v, vUnit);
            }
            break;
        case 1057: // Version info: has real merged data?
            if (size >= 5)
            {
                r.Skip(4);
                doc.hasRealMergedData = r.U8();
            }
            break;
        default:
            break;
        }
        position = dataEnd + (size & 1);
    }
}

// --- Fonts: parsed from the text engine data inside 'TySh' blocks --------------------------------

std::wstring DecodePdfString(const std::vector<uint8_t>& raw)
{
    if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF)
    {
        return FromUtf16BE(raw.data() + 2, raw.size() - 2);
    }
    std::wstring text;
    for (uint8_t b : raw)
    {
        text.push_back(static_cast<wchar_t>(b));
    }
    return text;
}

void AddFont(Document& doc, const std::wstring& name)
{
    if (name.empty() || name == L"AdobeInvisFont" || doc.fonts.size() >= kMaxFonts)
    {
        return;
    }
    if (std::find(doc.fonts.begin(), doc.fonts.end(), name) == doc.fonts.end())
    {
        doc.fonts.push_back(name);
    }
}

void ExtractFonts(const std::vector<uint8_t>& data, Document& doc)
{
    static constexpr char kFontSet[] = "/FontSet";
    const size_t size = data.size();
    auto it = data.begin();
    while (true)
    {
        it = std::search(it, data.end(), std::begin(kFontSet), std::end(kFontSet) - 1);
        if (it == data.end())
        {
            return;
        }
        size_t i = static_cast<size_t>(it - data.begin()) + (sizeof(kFontSet) - 1);
        while (i < size && data[i] != '[' && data[i] != '/')
        {
            ++i;
        }
        if (i >= size || data[i] != '[')
        {
            it = data.begin() + static_cast<std::ptrdiff_t>(std::min(i, size));
            continue;
        }
        ++i;
        int depth = 1;
        std::string lastName;
        while (i < size && depth > 0)
        {
            const uint8_t c = data[i];
            if (c == '(')
            {
                std::vector<uint8_t> raw;
                ++i;
                while (i < size && data[i] != ')')
                {
                    if (data[i] == '\\' && i + 1 < size)
                    {
                        ++i;
                    }
                    raw.push_back(data[i]);
                    ++i;
                }
                if (lastName == "Name")
                {
                    AddFont(doc, DecodePdfString(raw));
                }
                lastName.clear();
            }
            else if (c == '/')
            {
                lastName.clear();
                ++i;
                while (i < size && std::isalnum(data[i]))
                {
                    lastName.push_back(static_cast<char>(data[i]));
                    ++i;
                }
                continue;
            }
            else if (c == '[')
            {
                ++depth;
            }
            else if (c == ']')
            {
                --depth;
            }
            ++i;
        }
        it = data.begin() + static_cast<std::ptrdiff_t>(std::min(i, size));
    }
}

// --- Layers -------------------------------------------------------------------------------------

enum class LayerKind
{
    Normal,
    GroupStart, // 'lsct' type 1/2: the group's own row in the Layers panel
    GroupEnd,   // 'lsct' type 3: hidden "</Layer group>" marker
};

struct RawLayer
{
    std::wstring name;
    LayerKind kind = LayerKind::Normal;
};

// After a tagged block, the next one may start at the exact end, or padded to 2 or 4 bytes.
void SeekNextBlock(Reader& r, uint64_t end, uint64_t limit)
{
    const uint64_t candidates[] = {end, (end + 1) & ~uint64_t(1), (end + 3) & ~uint64_t(3)};
    for (uint64_t candidate : candidates)
    {
        if (candidate + 4 <= limit)
        {
            r.Seek(candidate);
            if (IsBlockSignature(r.U32()))
            {
                r.Seek(candidate);
                return;
            }
        }
    }
    r.Seek(std::min(end, limit));
}

void ParseLayerRecords(Reader& r, Document& doc, std::vector<RawLayer>& layers)
{
    const int count = std::abs(static_cast<int>(r.I16()));
    if (count > kMaxLayers)
    {
        throw FormatError("too many layers");
    }
    for (int index = 0; index < count; ++index)
    {
        r.Skip(16); // bounds
        const uint16_t channelCount = r.U16();
        if (channelCount > 64)
        {
            throw FormatError("bad channel count");
        }
        r.Skip(uint64_t(channelCount) * (doc.isPsb ? 10 : 6));
        if (r.U32() != Tag("8BIM"))
        {
            throw FormatError("bad blend signature");
        }
        r.Skip(8); // blend key, opacity, clipping, flags, filler
        const uint32_t extraLength = r.U32();
        const uint64_t extraEnd = r.Pos() + extraLength;
        if (extraEnd > r.Length())
        {
            throw FormatError("layer record past end");
        }
        r.Skip(r.U32()); // layer mask
        r.Skip(r.U32()); // blending ranges
        const uint8_t nameLength = r.U8();
        RawLayer layer;
        layer.name = FromAnsi(r.Bytes(nameLength));
        const uint64_t nameTotal = 1 + uint64_t(nameLength);
        r.Skip(((nameTotal + 3) & ~uint64_t(3)) - nameTotal);

        while (r.Pos() + 12 <= extraEnd)
        {
            const uint64_t blockStart = r.Pos();
            if (!IsBlockSignature(r.U32()))
            {
                r.Seek(blockStart);
                break;
            }
            const uint32_t key = r.U32();
            const uint64_t length = (doc.isPsb && IsLongKey(key)) ? r.U64() : r.U32();
            const uint64_t dataStart = r.Pos();
            if (length > extraEnd - dataStart)
            {
                break;
            }
            if (key == Tag("luni") && length >= 4)
            {
                const uint64_t chars = r.U32();
                if (chars * 2 <= length - 4)
                {
                    const std::vector<uint8_t> utf16 = r.Bytes(chars * 2);
                    std::wstring unicodeName = FromUtf16BE(utf16.data(), utf16.size());
                    if (!unicodeName.empty())
                    {
                        layer.name = std::move(unicodeName);
                    }
                }
            }
            else if ((key == Tag("lsct") || key == Tag("lsdk")) && length >= 4)
            {
                const uint32_t type = r.U32();
                if (type == 1 || type == 2)
                {
                    layer.kind = LayerKind::GroupStart;
                }
                else if (type == 3)
                {
                    layer.kind = LayerKind::GroupEnd;
                }
            }
            else if (key == Tag("TySh") && length <= kMaxTextBlockBytes)
            {
                ++doc.textLayerCount;
                ExtractFonts(r.Bytes(length), doc);
            }
            SeekNextBlock(r, dataStart + length, extraEnd);
        }
        r.Seek(extraEnd);
        layers.push_back(std::move(layer));
    }
}

void ParseLayers(Reader& r, Document& doc, uint64_t sectionEnd)
{
    std::vector<RawLayer> layers;
    const uint64_t infoLength = doc.isPsb ? r.U64() : r.U32();
    const uint64_t infoEnd = r.Pos() + infoLength;
    if (infoEnd > sectionEnd)
    {
        throw FormatError("layer info past end");
    }

    if (infoLength > 0)
    {
        ParseLayerRecords(r, doc, layers);
    }
    else
    {
        // 16/32-bit documents keep their layers in a global 'Lr16' / 'Lr32' block instead.
        r.Seek(infoEnd);
        if (r.Pos() + 4 <= sectionEnd)
        {
            r.Skip(r.U32()); // global layer mask info
        }
        while (r.Pos() + 12 <= sectionEnd)
        {
            if (!IsBlockSignature(r.U32()))
            {
                break;
            }
            const uint32_t key = r.U32();
            const uint64_t length = (doc.isPsb && IsLongKey(key)) ? r.U64() : r.U32();
            const uint64_t dataStart = r.Pos();
            if (length > sectionEnd - dataStart)
            {
                break;
            }
            if (key == Tag("Lr16") || key == Tag("Lr32") || key == Tag("Layr"))
            {
                ParseLayerRecords(r, doc, layers);
                break;
            }
            SeekNextBlock(r, dataStart + length, sectionEnd);
        }
    }

    // Records are stored bottom-to-top; walk top-to-bottom like the Layers panel.
    int depth = 0;
    for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer)
    {
        switch (layer->kind)
        {
        case LayerKind::GroupStart:
            ++doc.layerCount;
            ++doc.groupCount;
            if (depth == 0 && doc.topLevel.size() < kMaxTopLevel)
            {
                doc.topLevel.push_back({layer->name, true});
            }
            ++depth;
            break;
        case LayerKind::GroupEnd:
            depth = std::max(0, depth - 1);
            break;
        case LayerKind::Normal:
            ++doc.layerCount;
            if (depth == 0 && doc.topLevel.size() < kMaxTopLevel)
            {
                doc.topLevel.push_back({layer->name, false});
            }
            break;
        }
    }
    doc.layersParsed = true;
}

// --- Composite ----------------------------------------------------------------------------------

void UnpackBits(const uint8_t* in, size_t inLength, uint8_t* out, size_t outLength)
{
    size_t i = 0;
    size_t o = 0;
    while (i < inLength && o < outLength)
    {
        const int header = static_cast<int8_t>(in[i++]);
        if (header >= 0)
        {
            const size_t count = std::min({size_t(header) + 1, inLength - i, outLength - o});
            std::memcpy(out + o, in + i, count);
            i += count;
            o += count;
        }
        else if (header != -128)
        {
            if (i >= inLength)
            {
                break;
            }
            const size_t count = std::min(size_t(1 - header), outLength - o);
            std::memset(out + o, in[i++], count);
            o += count;
        }
    }
    if (o < outLength)
    {
        std::memset(out + o, 0, outLength - o);
    }
}

uint8_t LinearToSrgb8(double v)
{
    if (!(v > 0))
    {
        return 0;
    }
    if (v >= 1)
    {
        return 255;
    }
    const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
}

uint8_t SampleChannel(const uint8_t* row, uint64_t x, int depth)
{
    switch (depth)
    {
    case 8:
        return row[x];
    case 16:
        return row[x * 2]; // big-endian: high byte
    case 32:
    {
        const uint8_t* p = row + x * 4;
        const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return LinearToSrgb8(value);
    }
    case 1:
        return ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;
    default:
        return 0;
    }
}

void LabToRgb(uint8_t l8, uint8_t a8, uint8_t b8, uint8_t* rgb)
{
    const double L = l8 * 100.0 / 255.0;
    const double A = double(a8) - 128.0;
    const double B = double(b8) - 128.0;
    const double fy = (L + 16.0) / 116.0;
    const double fx = fy + A / 500.0;
    const double fz = fy - B / 200.0;
    auto inverse = [](double t) {
        const double t3 = t * t * t;
        return t3 > 0.008856 ? t3 : (t - 16.0 / 116.0) / 7.787;
    };
    const double X = 0.9642 * inverse(fx);
    const double Y = 1.0 * inverse(fy);
    const double Z = 0.8249 * inverse(fz);
    // D50 XYZ -> linear sRGB (Bradford-adapted)
    const double r = 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z;
    const double g = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z;
    const double b = 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z;
    rgb[0] = LinearToSrgb8(r);
    rgb[1] = LinearToSrgb8(g);
    rgb[2] = LinearToSrgb8(b);
}
} // namespace

bool IsPsd(const uint8_t* header, size_t size)
{
    return size >= 4 && header[0] == '8' && header[1] == 'B' && header[2] == 'P' && header[3] == 'S';
}

const wchar_t* ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::Bitmap: return L"Bitmap";
    case Mode::Grayscale: return L"Grayscale";
    case Mode::Indexed: return L"Indexed";
    case Mode::Rgb: return L"RGB";
    case Mode::Cmyk: return L"CMYK";
    case Mode::Multichannel: return L"Multichannel";
    case Mode::Duotone: return L"Duotone";
    case Mode::Lab: return L"Lab";
    }
    return L"Unknown";
}

Document ReadDocument(Reader& r, bool withLayers)
{
    r.Seek(0);
    uint8_t signature[4];
    r.Read(signature, 4);
    if (!IsPsd(signature, 4))
    {
        throw FormatError("not a PSD file");
    }
    const uint16_t version = r.U16();
    if (version != 1 && version != 2)
    {
        throw FormatError("unsupported PSD version");
    }

    Document doc;
    doc.isPsb = version == 2;
    r.Skip(6);
    doc.channels = r.U16();
    doc.height = r.U32();
    doc.width = r.U32();
    doc.depth = r.U16();
    doc.mode = static_cast<Mode>(r.U16());

    const uint32_t maxDimension = doc.isPsb ? 300000 : 30000;
    if (doc.channels < 1 || doc.channels > 56 || doc.width < 1 || doc.height < 1 || doc.width > maxDimension ||
        doc.height > maxDimension || (doc.depth != 1 && doc.depth != 8 && doc.depth != 16 && doc.depth != 32))
    {
        throw FormatError("impossible header values");
    }

    const uint32_t colorModeLength = r.U32();
    if (doc.mode == Mode::Indexed && colorModeLength >= 768)
    {
        doc.palette = r.Bytes(768);
        r.Skip(colorModeLength - 768);
    }
    else
    {
        r.Skip(colorModeLength);
    }

    const uint32_t resourcesLength = r.U32();
    const uint64_t resourcesStart = r.Pos();
    const uint64_t resourcesEnd = resourcesStart + resourcesLength;
    if (resourcesEnd > r.Length())
    {
        throw FormatError("image resources truncated");
    }
    ReadResources(r, resourcesStart, resourcesEnd, doc);

    // Layer and mask information section: only its length is needed to find the composite.
    r.Seek(resourcesEnd);
    if (r.Pos() + (doc.isPsb ? 8 : 4) <= r.Length())
    {
        const uint64_t sectionLength = doc.isPsb ? r.U64() : r.U32();
        const uint64_t sectionStart = r.Pos();
        if (sectionLength <= r.Length() - sectionStart)
        {
            const uint64_t sectionEnd = sectionStart + sectionLength;
            if (sectionEnd + 2 <= r.Length())
            {
                doc.imageDataOffset = sectionEnd;
            }
            if (withLayers && sectionLength > 0)
            {
                try
                {
                    ParseLayers(r, doc, sectionEnd);
                }
                catch (const FormatError&)
                {
                    // Keep header info even if layer records are damaged.
                }
            }
            else if (withLayers)
            {
                doc.layersParsed = true; // flattened image: zero layers
            }
        }
    }
    return doc;
}

Image RenderComposite(Reader& r, const Document& doc, int maxSize, int maxSupersample)
{
    if (doc.imageDataOffset == 0 || doc.hasRealMergedData == 0)
    {
        return {};
    }
    int need = 1;
    if (doc.mode == Mode::Rgb || doc.mode == Mode::Lab)
    {
        need = 3;
    }
    else if (doc.mode == Mode::Cmyk)
    {
        need = 4;
    }
    if (doc.channels < need || (doc.mode == Mode::Bitmap) != (doc.depth == 1))
    {
        return {};
    }

    const uint64_t W = doc.width;
    const uint64_t H = doc.height;
    const uint64_t rowBytes = doc.depth == 1 ? (W + 7) / 8 : W * uint64_t(doc.depth / 8);

    r.Seek(doc.imageDataOffset);
    const uint16_t compression = r.U16();
    if (compression > 1)
    {
        return {};
    }

    const auto [tw, th] = FitInside(W, H, maxSize, false);
    const int k = std::clamp(static_cast<int>(std::max(W / uint64_t(tw), H / uint64_t(th))), 1, maxSupersample);

    uint64_t dataStart = r.Pos();
    std::vector<uint64_t> rowOffsets;
    std::vector<uint32_t> rowLengths;
    if (compression == 1)
    {
        const uint64_t countSize = doc.isPsb ? 4 : 2;
        const uint64_t neededRows = uint64_t(need) * H;
        const std::vector<uint8_t> table = r.Bytes(neededRows * countSize);
        dataStart = doc.imageDataOffset + 2 + uint64_t(doc.channels) * H * countSize;
        rowOffsets.resize(static_cast<size_t>(neededRows));
        rowLengths.resize(static_cast<size_t>(neededRows));
        uint64_t offset = dataStart;
        for (size_t i = 0; i < rowOffsets.size(); ++i)
        {
            const uint8_t* p = table.data() + i * countSize;
            const uint32_t length = countSize == 2 ? uint32_t((p[0] << 8) | p[1])
                                                   : (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                                         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            rowOffsets[i] = offset;
            rowLengths[i] = length;
            offset += length;
        }
        if (offset > r.Length())
        {
            return {}; // composite truncated
        }
    }
    else if (dataStart + uint64_t(need) * H * rowBytes > r.Length())
    {
        return {};
    }

    std::vector<uint8_t> rows(static_cast<size_t>(rowBytes) * need);
    std::vector<uint8_t> packed;
    auto loadRow = [&](int channel, uint64_t y) {
        uint8_t* destination = rows.data() + size_t(channel) * rowBytes;
        if (compression == 0)
        {
            r.Seek(dataStart + (uint64_t(channel) * H + y) * rowBytes);
            r.Read(destination, static_cast<size_t>(rowBytes));
            return;
        }
        const size_t index = static_cast<size_t>(uint64_t(channel) * H + y);
        const uint32_t length = rowLengths[index];
        if (length > rowBytes * 2 + 128)
        {
            throw FormatError("bad RLE row");
        }
        packed.resize(length);
        r.Seek(rowOffsets[index]);
        if (length > 0)
        {
            r.Read(packed.data(), length);
        }
        UnpackBits(packed.data(), length, destination, static_cast<size_t>(rowBytes));
    };

    std::vector<uint64_t> xs(size_t(tw) * k);
    for (int tx = 0; tx < tw; ++tx)
    {
        for (int i = 0; i < k; ++i)
        {
            const double fx = (double(tx) * k + i + 0.5) * double(W) / (double(tw) * k);
            xs[size_t(tx) * k + i] = std::min<uint64_t>(W - 1, static_cast<uint64_t>(fx));
        }
    }

    Image image;
    image.width = tw;
    image.height = th;
    image.bgra.resize(size_t(tw) * th * 4);
    std::vector<uint32_t> sums(size_t(tw) * 3);
    const uint32_t samples = uint32_t(k) * uint32_t(k);
    const uint8_t* planes[4] = {};
    for (int c = 0; c < need; ++c)
    {
        planes[c] = rows.data() + size_t(c) * rowBytes;
    }

    for (int ty = 0; ty < th; ++ty)
    {
        std::fill(sums.begin(), sums.end(), 0u);
        for (int j = 0; j < k; ++j)
        {
            const double fy = (double(ty) * k + j + 0.5) * double(H) / (double(th) * k);
            const uint64_t sy = std::min<uint64_t>(H - 1, static_cast<uint64_t>(fy));
            for (int c = 0; c < need; ++c)
            {
                loadRow(c, sy);
            }
            for (int tx = 0; tx < tw; ++tx)
            {
                uint32_t* sum = sums.data() + size_t(tx) * 3;
                for (int i = 0; i < k; ++i)
                {
                    const uint64_t sx = xs[size_t(tx) * k + i];
                    uint8_t rgb[3];
                    switch (doc.mode)
                    {
                    case Mode::Rgb:
                        rgb[0] = SampleChannel(planes[0], sx, doc.depth);
                        rgb[1] = SampleChannel(planes[1], sx, doc.depth);
                        rgb[2] = SampleChannel(planes[2], sx, doc.depth);
                        break;
                    case Mode::Cmyk:
                    {
                        // Stored inverted (255 = no ink). Simple conversion, no ICC profile.
                        const uint32_t kk = SampleChannel(planes[3], sx, doc.depth);
                        rgb[0] = uint8_t(SampleChannel(planes[0], sx, doc.depth) * kk / 255);
                        rgb[1] = uint8_t(SampleChannel(planes[1], sx, doc.depth) * kk / 255);
                        rgb[2] = uint8_t(SampleChannel(planes[2], sx, doc.depth) * kk / 255);
                        break;
                    }
                    case Mode::Lab:
                        LabToRgb(SampleChannel(planes[0], sx, doc.depth), SampleChannel(planes[1], sx, doc.depth),
                                 SampleChannel(planes[2], sx, doc.depth), rgb);
                        break;
                    case Mode::Indexed:
                    {
                        const uint8_t index = planes[0][sx];
                        if (doc.palette.size() >= 768)
                        {
                            rgb[0] = doc.palette[index];
                            rgb[1] = doc.palette[256 + index];
                            rgb[2] = doc.palette[512 + index];
                        }
                        else
                        {
                            rgb[0] = rgb[1] = rgb[2] = index;
                        }
                        break;
                    }
                    default:
                        rgb[0] = rgb[1] = rgb[2] = SampleChannel(planes[0], sx, doc.depth);
                        break;
                    }
                    sum[0] += rgb[0];
                    sum[1] += rgb[1];
                    sum[2] += rgb[2];
                }
            }
        }
        uint8_t* out = image.bgra.data() + size_t(ty) * tw * 4;
        for (int tx = 0; tx < tw; ++tx)
        {
            const uint32_t* sum = sums.data() + size_t(tx) * 3;
            out[tx * 4 + 0] = uint8_t(sum[2] / samples);
            out[tx * 4 + 1] = uint8_t(sum[1] / samples);
            out[tx * 4 + 2] = uint8_t(sum[0] / samples);
            out[tx * 4 + 3] = 255;
        }
    }
    return image;
}
} // namespace dp::psd
