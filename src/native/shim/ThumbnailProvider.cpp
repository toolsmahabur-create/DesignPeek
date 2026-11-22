#include "ThumbnailProvider.h"

#include <algorithm>
#include <cstring>

#include "Module.h"
#include "WorkerHost.h"

namespace
{
constexpr DWORD kThumbnailTimeoutMs = 3000;
constexpr UINT kMinSize = 16;
constexpr UINT kMaxSize = 2560;

HBITMAP CreateBitmapFromPixels(const WorkerBitmap& image)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits)
    {
        std::memcpy(bits, image.pixels.data(), image.pixels.size());
    }
    return bitmap;
}
} // namespace

ThumbnailProvider::ThumbnailProvider() noexcept { DllAddRef(); }

ThumbnailProvider::~ThumbnailProvider()
{
    if (m_stream)
    {
        m_stream->Release();
    }
    DllRelease();
}

IFACEMETHODIMP ThumbnailProvider::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
    {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == __uuidof(IInitializeWithStream))
    {
        *ppv = static_cast<IInitializeWithStream*>(this);
    }
    else if (riid == __uuidof(IThumbnailProvider))
    {
        *ppv = static_cast<IThumbnailProvider*>(this);
    }
    else
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::AddRef() { return InterlockedIncrement(&m_refCount); }

IFACEMETHODIMP_(ULONG) ThumbnailProvider::Release()
{
    const LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0)
    {
        delete this;
    }
    return count;
}

IFACEMETHODIMP ThumbnailProvider::Initialize(IStream* stream, DWORD)
{
    if (!stream)
    {
        return E_INVALIDARG;
    }
    if (m_stream)
    {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }
    m_stream = stream;
    m_stream->AddRef();
    return S_OK;
}

IFACEMETHODIMP ThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType)
{
    if (!bitmap || !alphaType)
    {
        return E_POINTER;
    }
    *bitmap = nullptr;
    *alphaType = WTSAT_UNKNOWN;
    if (!m_stream)
    {
        return E_UNEXPECTED;
    }

    cx = std::clamp(cx, kMinSize, kMaxSize);

    WorkerBitmap image;
    const HRESULT hr = RunThumbnailWorker(m_stream, cx, kThumbnailTimeoutMs, image);
    if (FAILED(hr))
    {
        return hr; // Explorer keeps showing the normal file icon.
    }

    HBITMAP result = CreateBitmapFromPixels(image);
    if (!result)
    {
        return E_OUTOFMEMORY;
    }
    *bitmap = result;
    *alphaType = image.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
    return S_OK;
}
