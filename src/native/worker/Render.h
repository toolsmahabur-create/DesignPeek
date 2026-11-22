#pragma once
#include <string>

#include "Image.h"
#include "Psd.h"
#include "Reader.h"

namespace dp
{
// Explorer thumbnail: embedded thumbnail when it is big enough (fastest), otherwise a sharp
// render from the merged composite. Empty image = keep Explorer's normal icon.
Image RenderThumbnail(Reader& reader, int size);

// Hover panel: sharp preview plus design info ("key\tvalue\n" lines).
Image RenderPreview(Reader& reader, int size, std::wstring& info);

std::wstring BuildInfo(const psd::Document& document);
} // namespace dp
