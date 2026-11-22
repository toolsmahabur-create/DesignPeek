#include "Wic.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace dp
{
Image DecodeImageToFit(const std::vector<uint8_t>& encoded, int maxSize, bool allowUpscale, bool swapRedBlue)
{
    if (encoded.empty() || encoded.size() > MAXDWORD)
    {
        return {};
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
    {
        return {};
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size()))))
    {
        return {};
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        return {};
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        return {};
    }

    const auto [targetWidth, targetHeight] = FitInside(width, height, maxSize, allowUpscale);
    ComPtr<IWICBitmapSource> source = frame;
    if (UINT(targetWidth) != width || UINT(targetHeight) != height)
    {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), UINT(targetWidth), UINT(targetHeight),
                                      WICBitmapInterpolationModeHighQualityCubic)))
        {
            return {};
        }
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
    {
        return {};
    }

    Image image;
    image.width = targetWidth;
    image.height = targetHeight;
    image.bgra.resize(size_t(targetWidth) * targetHeight * 4);
    if (FAILED(converter->CopyPixels(nullptr, UINT(targetWidth) * 4, static_cast<UINT>(image.bgra.size()),
                                     image.bgra.data())))
    {
        return {};
    }
    for (size_t i = 0; i < image.bgra.size(); i += 4)
    {
        if (swapRedBlue)
        {
            std::swap(image.bgra[i], image.bgra[i + 2]);
        }
        image.bgra[i + 3] = 255;
    }
    return image;
}
} // namespace dp
