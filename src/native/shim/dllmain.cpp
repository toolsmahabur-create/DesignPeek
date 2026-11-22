#include <windows.h>
#include <unknwn.h>
#include <new>

#include "Guids.h"
#include "Module.h"
#include "ThumbnailProvider.h"

HMODULE g_hModule = nullptr;
static volatile LONG g_dllRefCount = 0;

void DllAddRef() noexcept { InterlockedIncrement(&g_dllRefCount); }
void DllRelease() noexcept { InterlockedDecrement(&g_dllRefCount); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

namespace
{
class ClassFactory final : public IClassFactory
{
public:
    ClassFactory() noexcept { DllAddRef(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer)
        {
            return CLASS_E_NOAGGREGATION;
        }
        auto* provider = new (std::nothrow) ThumbnailProvider();
        if (!provider)
        {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock)
        {
            DllAddRef();
        }
        else
        {
            DllRelease();
        }
        return S_OK;
    }

private:
    ~ClassFactory() { DllRelease(); }
    LONG m_refCount = 1;
};
} // namespace

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv)
    {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (rclsid != CLSID_DesignPeekThumbnailProvider)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory)
    {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return g_dllRefCount > 0 ? S_FALSE : S_OK;
}
