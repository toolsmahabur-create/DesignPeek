#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <thumbcache.h>

// IThumbnailProvider for PSD/PSB (later also AI/PDF).
// Windows creates it inside its isolated thumbnail process and hands us an IStream.
class ThumbnailProvider final : public IInitializeWithStream, public IThumbnailProvider
{
public:
    ThumbnailProvider() noexcept;

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override;

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType) override;

private:
    ~ThumbnailProvider();

    LONG m_refCount = 1;
    IStream* m_stream = nullptr;
};
