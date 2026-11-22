#include "Render.h"

#include <algorithm>
#include <cwchar>

#include "Wic.h"

namespace dp
{
namespace
{
bool IsPsdFile(Reader& r)
{
    if (r.Length() < 4)
    {
        return false;
    }
    uint8_t header[4];
    r.Seek(0);
    r.Read(header, 4);
    return psd::IsPsd(header, 4);
}

Image DecodeEmbedded(const psd::Document& doc, int size)
{
    return doc.thumbJpeg.empty() ? Image{} : DecodeImageToFit(doc.thumbJpeg, size, true, doc.thumbIsBgr);
}

Image TryComposite(Reader& r, const psd::Document& doc, int size)
{
    try
    {
        return psd::RenderComposite(r, doc, size, 2);
    }
    catch (const FormatError&)
    {
        return {};
    }
}

std::wstring Clean(std::wstring text)
{
    std::replace_if(text.begin(), text.end(), [](wchar_t c) { return c == L'\t' || c == L'\r' || c == L'\n'; }, L' ');
    return text;
}

void Line(std::wstring& out, const wchar_t* key, const std::wstring& value)
{
    out += key;
    out += L'\t';
    out += Clean(value);
    out += L'\n';
}
} // namespace

Image RenderThumbnail(Reader& r, int size)
{
    if (!IsPsdFile(r))
    {
        return {};
    }
    const psd::Document doc = psd::ReadDocument(r, false);
    const int embeddedSize = std::max(doc.thumbWidth, doc.thumbHeight);
    if (!doc.thumbJpeg.empty() && size <= embeddedSize + 32)
    {
        Image image = DecodeEmbedded(doc, size);
        if (!image.Empty())
        {
            return image;
        }
    }
    Image composite = TryComposite(r, doc, size);
    return composite.Empty() ? DecodeEmbedded(doc, size) : composite;
}

Image RenderPreview(Reader& r, int size, std::wstring& info)
{
    if (!IsPsdFile(r))
    {
        return {};
    }
    const psd::Document doc = psd::ReadDocument(r, true);
    info = BuildInfo(doc);
    Image image = TryComposite(r, doc, size);
    if (image.Empty())
    {
        Line(info, L"note", L"no-composite");
        image = DecodeEmbedded(doc, size);
    }
    return image;
}

std::wstring BuildInfo(const psd::Document& doc)
{
    std::wstring info;
    Line(info, L"format", doc.isPsb ? L"PSB" : L"PSD");
    Line(info, L"width", std::to_wstring(doc.width));
    Line(info, L"height", std::to_wstring(doc.height));
    if (doc.dpiX > 0)
    {
        wchar_t dpi[32];
        swprintf_s(dpi, L"%.0f", doc.dpiX);
        Line(info, L"dpi", dpi);
    }
    Line(info, L"mode", psd::ModeName(doc.mode));
    Line(info, L"depth", std::to_wstring(doc.depth));
    if (doc.layersParsed)
    {
        Line(info, L"layers", std::to_wstring(doc.layerCount));
        Line(info, L"groups", std::to_wstring(doc.groupCount));
        Line(info, L"textlayers", std::to_wstring(doc.textLayerCount));
        for (const auto& item : doc.topLevel)
        {
            Line(info, item.isGroup ? L"group" : L"layer", item.name);
        }
        for (const auto& font : doc.fonts)
        {
            Line(info, L"font", font);
        }
    }
    return info;
}
} // namespace dp
