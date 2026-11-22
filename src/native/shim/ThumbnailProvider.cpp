#include "ThumbnailProvider.h"

#include <algorithm>
#include <cstring>

#include "../common/WorkerClient.h"
#include "Module.h"

namespace
{
constexpr DWORD kThumbnailTimeoutMs = 3000;
constexpr UINT kMinSize = 16;
constexpr UINT kMaxSize = 2560;

int32_t ReadFromStream(IStream* stream, int64_t offset, uint8_t* destination, int32_t count)
{
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr)))
    {
        return -1;
    }
    int32_t total = 0;
    while (total < count)
    {
        ULONG read = 0;
        const HRESULT hr = stream->Read(destination + total, static_cast<ULONG>(count - total), &read);
        if (FAILED(hr))
        {
            return total > 0 ? total : -1;
        }
        total += static_cast<int32_t>(read);
        if (hr == S_FALSE || read == 0)
        {
            break;
        }
    }
    return total;
}

HBITMAP CreateBitmapFromPixels(const dp::WorkerResponse& image)
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

    STATSTG stat{};
    HRESULT hr = m_stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr))
    {
        return hr;
    }
    if (stat.cbSize.QuadPart < 4)
    {
        return E_FAIL; // zero-byte file: keep the normal icon
    }

    cx = std::clamp(cx, kMinSize, kMaxSize);
    IStream* stream = m_stream;

    dp::WorkerRequest request;
    request.workerPath = dp::SiblingPath(g_hModule, L"DesignPeek.Worker.exe");
    request.arguments = L"thumb --size " + std::to_wstring(cx) + L" --length " + std::to_wstring(stat.cbSize.QuadPart);
    request.length = stat.cbSize.QuadPart;
    request.timeoutMs = kThumbnailTimeoutMs;
    request.maxDimension = static_cast<int>(cx);
    request.read = [stream](int64_t offset, uint8_t* destination, int32_t count) {
        return ReadFromStream(stream, offset, destination, count);
    };

    dp::WorkerResponse image;
    hr = dp::RunWorker(request, image);
    if (hr != S_OK)
    {
        return FAILED(hr) ? hr : E_FAIL; // Explorer keeps showing the normal file icon
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
