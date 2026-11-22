#include "Resolver.h"

#include <exdisp.h>
#include <propkey.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <servprov.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "Preview.h"

using Microsoft::WRL::ComPtr;

namespace dp
{
namespace
{
// SID_STopLevelBrowser {4C96BE40-915C-11CF-99D3-00AA004AE837}
constexpr GUID kSidTopLevelBrowser = {0x4C96BE40, 0x915C, 0x11CF, {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};
// PKEY_ItemTypeText {B725F130-47EF-101A-A5F1-02608C9EEBAC}, 4
constexpr PROPERTYKEY kItemTypeText = {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 4};

constexpr int kMaxViewItems = 5000;
constexpr int kMaxParentLevels = 6;

class Bstr
{
public:
    ~Bstr() { SysFreeString(m_value); }
    BSTR* operator&() { return &m_value; }
    std::wstring str() const { return m_value ? std::wstring(m_value, SysStringLen(m_value)) : std::wstring(); }

private:
    BSTR m_value = nullptr;
};

std::wstring ItemTypeOf(IShellItem* item)
{
    ComPtr<IShellItem2> item2;
    PWSTR text = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2))) && SUCCEEDED(item2->GetString(kItemTypeText, &text)))
    {
        result = text;
        CoTaskMemFree(text);
    }
    return result;
}

std::wstring DisplayName(IShellItem* item, SIGDN kind)
{
    PWSTR text = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(kind, &text)))
    {
        result = text;
        CoTaskMemFree(text);
    }
    return result;
}

bool IsFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FolderPathOf(IFolderView2* view)
{
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    std::wstring result;
    if (SUCCEEDED(view->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&pidl)))
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path)))
        {
            result = path;
            CoTaskMemFree(path);
        }
        CoTaskMemFree(pidl);
    }
    return result;
}
} // namespace

bool Resolver::Initialize()
{
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_automation))) &&
        FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_automation))))
    {
        return false;
    }
    return SUCCEEDED(m_automation->get_ControlViewWalker(&m_walker));
}

std::wstring Resolver::Resolve(POINT screenPoint, HWND itemsView)
{
    if (!m_automation)
    {
        return {};
    }

    // 1. Which row is under the mouse?
    ComPtr<IUIAutomationElement> element;
    if (FAILED(m_automation->ElementFromPoint(screenPoint, &element)) || !element)
    {
        return {};
    }
    for (int level = 0; element && level < kMaxParentLevels; ++level)
    {
        CONTROLTYPEID type = 0;
        if (FAILED(element->get_CurrentControlType(&type)))
        {
            return {};
        }
        if (type == UIA_ListItemControlTypeId)
        {
            break;
        }
        if (type == UIA_ListControlTypeId || type == UIA_PaneControlTypeId || type == UIA_WindowControlTypeId)
        {
            return {}; // empty space between items
        }
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(m_walker->GetParentElement(element.Get(), &parent)))
        {
            return {};
        }
        element = parent;
    }
    if (!element)
    {
        return {};
    }
    CONTROLTYPEID finalType = 0;
    element->get_CurrentControlType(&finalType);
    if (finalType != UIA_ListItemControlTypeId)
    {
        return {};
    }

    Bstr name;
    Bstr itemType;
    element->get_CurrentName(&name);
    element->get_CurrentItemType(&itemType);
    const std::wstring itemName = name.str();
    if (itemName.empty())
    {
        return {};
    }

    // 2. Which folder is this Explorer tab showing?
    ComPtr<IFolderView2> view = FindFolderView(GetParent(itemsView));
    if (!view)
    {
        return {};
    }
    const std::wstring folder = FolderPathOf(view.Get());
    if (!folder.empty())
    {
        return MatchInFolder(folder, itemName, itemType.str());
    }
    return MatchInView(view.Get(), itemName, itemType.str()); // libraries, search results, ...
}

ComPtr<IFolderView2> Resolver::FindFolderView(HWND defView)
{
    if (!defView)
    {
        return nullptr;
    }
    for (auto it = m_views.begin(); it != m_views.end();)
    {
        it = IsWindow(it->first) ? std::next(it) : m_views.erase(it);
    }
    if (auto found = m_views.find(defView); found != m_views.end())
    {
        return found->second;
    }

    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
    {
        return nullptr;
    }
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count; ++i)
    {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> dispatch;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch)
        {
            continue;
        }
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> shellView;
        HWND viewWindow = nullptr;
        if (FAILED(dispatch.As(&services)) ||
            FAILED(services->QueryService(kSidTopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&shellView)) || FAILED(shellView->GetWindow(&viewWindow)))
        {
            continue;
        }
        if (viewWindow == defView)
        {
            ComPtr<IFolderView2> folderView;
            if (SUCCEEDED(shellView.As(&folderView)))
            {
                m_views[defView] = folderView;
                return folderView;
            }
        }
    }
    return nullptr;
}

std::wstring Resolver::MatchInFolder(const std::wstring& folder, const std::wstring& name, const std::wstring& itemType)
{
    std::wstring base = folder;
    if (!base.empty() && base.back() != L'\\')
    {
        base += L'\\';
    }

    // Extensions shown: the name is the file name.
    const std::wstring direct = base + name;
    if (IsSupportedFile(direct) && IsFile(direct))
    {
        return direct;
    }

    // Extensions hidden ("logo" may be logo.psd, logo.ai, a folder...): confirm with the item type.
    std::vector<std::wstring> candidates;
    for (const wchar_t* extension : {L".psd", L".psb"})
    {
        const std::wstring candidate = base + name + extension;
        if (IsFile(candidate))
        {
            candidates.push_back(candidate);
        }
    }
    for (const auto& candidate : candidates)
    {
        if (itemType.empty())
        {
            return candidate;
        }
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(candidate.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            ItemTypeOf(item.Get()) == itemType)
        {
            return candidate;
        }
    }
    return {};
}

std::wstring Resolver::MatchInView(IFolderView2* view, const std::wstring& name, const std::wstring& itemType)
{
    int count = 0;
    if (FAILED(view->ItemCount(SVGIO_ALLVIEW, &count)))
    {
        return {};
    }
    count = std::min(count, kMaxViewItems);
    for (int i = 0; i < count; ++i)
    {
        ComPtr<IShellItem> item;
        if (FAILED(view->GetItem(i, IID_PPV_ARGS(&item))) || DisplayName(item.Get(), SIGDN_NORMALDISPLAY) != name)
        {
            continue;
        }
        const std::wstring path = DisplayName(item.Get(), SIGDN_FILESYSPATH);
        if (IsSupportedFile(path) && (itemType.empty() || ItemTypeOf(item.Get()) == itemType))
        {
            return path;
        }
    }
    return {};
}
} // namespace dp
