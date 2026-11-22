// Loads DesignPeek.Shim.dll the same way Windows does (DllGetClassObject -> IInitializeWithStream ->
// IThumbnailProvider) and checks the result. Used by CI; needs no registry entries.
//
// usage: DesignPeek.ShimTest.exe <shim.dll> <file> <size> <ok|fail>
#include <windows.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <cstdio>
#include <cwchar>

#include "Guids.h"

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

namespace
{
constexpr ULONGLONG kMaxAllowedMs = 3500;

template <typename T>
void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5)
    {
        std::fwprintf(stderr, L"usage: DesignPeek.ShimTest <shim.dll> <file> <size> <ok|fail>\n");
        return 2;
    }

    const UINT size = static_cast<UINT>(_wtoi(argv[3]));
    const bool expectOk = std::wcscmp(argv[4], L"ok") == 0;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        return 3;
    }

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll)
    {
        std::wprintf(L"cannot load shim (error %lu)\n", GetLastError());
        return 3;
    }
    auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!getClassObject)
    {
        std::wprintf(L"DllGetClassObject not exported\n");
        return 3;
    }

    IClassFactory* factory = nullptr;
    IInitializeWithStream* initialize = nullptr;
    IThumbnailProvider* provider = nullptr;
    IStream* stream = nullptr;
    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    ULONGLONG elapsed = 0;

    HRESULT hr = getClassObject(CLSID_DesignPeekThumbnailProvider, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
    {
        hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&initialize));
    }
    if (SUCCEEDED(hr))
    {
        hr = SHCreateStreamOnFileEx(argv[2], STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                    &stream);
    }
    if (SUCCEEDED(hr))
    {
        hr = initialize->Initialize(stream, STGM_READ);
    }
    if (SUCCEEDED(hr))
    {
        hr = initialize->QueryInterface(IID_PPV_ARGS(&provider));
    }
    if (SUCCEEDED(hr))
    {
        const ULONGLONG start = GetTickCount64();
        hr = provider->GetThumbnail(size, &bitmap, &alpha);
        elapsed = GetTickCount64() - start;
    }

    BITMAP info{};
    if (bitmap)
    {
        GetObjectW(bitmap, sizeof(info), &info);
    }
    const bool success = SUCCEEDED(hr) && bitmap != nullptr;
    std::wprintf(L"  hr=0x%08lX  %ldx%ld  alpha=%d  %llu ms\n", static_cast<unsigned long>(hr), info.bmWidth,
                 info.bmHeight, static_cast<int>(alpha), elapsed);

    int exitCode = success == expectOk ? 0 : 1;
    if (success && (info.bmWidth > static_cast<LONG>(size) || info.bmHeight > static_cast<LONG>(size)))
    {
        std::wprintf(L"  thumbnail larger than requested\n");
        exitCode = 1;
    }
    if (elapsed > kMaxAllowedMs)
    {
        std::wprintf(L"  too slow\n");
        exitCode = 1;
    }

    if (bitmap)
    {
        DeleteObject(bitmap);
    }
    SafeRelease(provider);
    SafeRelease(initialize);
    SafeRelease(stream);
    SafeRelease(factory);
    CoUninitialize();
    return exitCode;
}
