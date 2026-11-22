#pragma once
#include <cstdint>
#include <vector>

#include "Image.h"

namespace dp
{
// Decodes JPEG/PNG/etc. bytes with Windows Imaging Component and scales them to fit maxSize.
Image DecodeImageToFit(const std::vector<uint8_t>& encoded, int maxSize, bool allowUpscale, bool swapRedBlue);
} // namespace dp
