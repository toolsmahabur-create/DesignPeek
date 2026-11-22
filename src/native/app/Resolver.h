#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>

namespace dp
{
// Finds which file is under the mouse in a File Explorer window.
// UI Automation gives the hovered row's name and type; the Explorer window's folder view
// (via IShellWindows) gives the folder, so we can turn that name into a full path.
// Must be used from one MTA thread.
class Resolver
{
public:
    bool Initialize();

    // itemsView: the DirectUIHWND under the cursor (child of SHELLDLL_DefView).
    // Returns the full path of a supported design file, or an empty string.
    std::wstring Resolve(POINT screenPoint, HWND itemsView);

private:
    Microsoft::WRL::ComPtr<IFolderView2> FindFolderView(HWND defView);
    std::wstring MatchInFolder(const std::wstring& folder, const std::wstring& name, const std::wstring& itemType);
    std::wstring MatchInView(IFolderView2* view, const std::wstring& name, const std::wstring& itemType);

    Microsoft::WRL::ComPtr<IUIAutomation> m_automation;
    Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> m_walker;
    std::unordered_map<HWND, Microsoft::WRL::ComPtr<IFolderView2>> m_views;
};
} // namespace dp
