// DesignPeek.exe — small background app (tray icon) that shows the hover side panel.
//
//   DesignPeek.exe                      run (single instance)
//   DesignPeek.exe --first-run          run and show a welcome notification
//   DesignPeek.exe --render-test <file> <out.txt>    CI self-test, no UI
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cwchar>
#include <memory>
#include <string>

#include "AppIcon.h"
#include "Panel.h"
#include "Pipeline.h"
#include "Preview.h"
#include "Strings.h"

namespace dp
{
namespace
{
constexpr wchar_t kAppClass[] = L"DesignPeekApp"; // the installer closes us by this class name
constexpr wchar_t kSettingsKey[] = L"Software\\DesignPeek";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DesignPeek";

constexpr UINT WM_DP_TRAY = WM_APP + 1;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kLoadingTimer = 2;
constexpr UINT kTickMs = 50;
constexpr ULONGLONG kHoverDelayMs = 250;     // mouse must rest this long on a file
constexpr ULONGLONG kLeaveHideMs = 450;      // hide after leaving Explorer
constexpr ULONGLONG kEmptyHideMs = 900;      // hide after resting on empty space / other files
constexpr UINT kLoadingDelayMs = 150;        // only show "Loading" for slow files

enum MenuId : UINT
{
    kMenuHover = 1,
    kMenuStartup,
    kMenuExit,
};

bool ClassIs(HWND window, const wchar_t* name)
{
    wchar_t buffer[64] = {};
    return window && GetClassNameW(window, buffer, ARRAYSIZE(buffer)) && wcscmp(buffer, name) == 0;
}

bool IsExplorerWindow(HWND root) { return ClassIs(root, L"CabinetWClass") || ClassIs(root, L"ExploreWClass"); }

bool IsItemsView(HWND window) { return ClassIs(window, L"DirectUIHWND") && ClassIs(GetParent(window), L"SHELLDLL_DefView"); }

bool AnyMouseButtonDown()
{
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}

class App
{
public:
    int Run(HINSTANCE instance, bool firstRun)
    {
        m_instance = instance;
        m_icon = CreateAppIcon(GetSystemMetrics(SM_CXSMICON));
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        LoadSettings();

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hIcon = m_icon;
        wc.lpszClassName = kAppClass;
        RegisterClassExW(&wc);
        m_window = CreateWindowExW(0, kAppClass, L"DesignPeek", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                   instance, this);
        if (!m_window || !m_panel.Create(instance))
        {
            return 1;
        }

        m_pipeline.Start(m_window);
        AddTrayIcon();
        if (firstRun)
        {
            ShowBalloon();
        }
        SetTimer(m_window, kTickTimer, kTickMs, nullptr);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            static_cast<App*>(create->lpCreateParams)->m_window = window;
        }
        auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == m_taskbarCreated && m_taskbarCreated != 0)
        {
            AddTrayIcon(); // Explorer restarted
            return 0;
        }
        switch (message)
        {
        case WM_TIMER:
            if (wParam == kTickTimer)
            {
                OnTick();
            }
            else if (wParam == kLoadingTimer)
            {
                KillTimer(m_window, kLoadingTimer);
                if (!m_pendingPath.empty())
                {
                    m_panel.ShowLoading(m_explorer, m_requestPoint, m_pendingPath);
                }
            }
            return 0;
        case WM_DP_RESOLVED:
        {
            std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lParam));
            OnResolved(static_cast<UINT>(wParam), *path);
            return 0;
        }
        case WM_DP_RENDERED:
        {
            std::unique_ptr<std::shared_ptr<PreviewData>> data(reinterpret_cast<std::shared_ptr<PreviewData>*>(lParam));
            OnRendered(static_cast<UINT>(wParam), *data);
            return 0;
        }
        case WM_DP_TRAY:
            switch (LOWORD(lParam))
            {
            case WM_CONTEXTMENU:
            case WM_LBUTTONUP:
            case NIN_SELECT:
            case NIN_KEYSELECT:
                ShowMenu();
                break;
            default:
                break;
            }
            return 0;
        case WM_ENDSESSION:
            if (wParam)
            {
                DestroyWindow(m_window);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(m_window);
            return 0;
        case WM_DESTROY:
        {
            KillTimer(m_window, kTickTimer);
            m_pipeline.Stop();
            NOTIFYICONDATAW data{sizeof(data)};
            data.hWnd = m_window;
            data.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &data);
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(m_window, message, wParam, lParam);
        }
    }

    void OnTick()
    {
        if (!m_hoverEnabled)
        {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        POINT point{};
        if (!GetCursorPos(&point))
        {
            return;
        }
        HWND under = WindowFromPoint(point);
        HWND panel = m_panel.Window();

        if (under && (under == panel || GetAncestor(under, GA_ROOT) == panel))
        {
            m_leaveHideAt = 0; // reading the panel or clicking its buttons
            m_lastPoint = point;
            return;
        }

        if (point.x != m_lastPoint.x || point.y != m_lastPoint.y)
        {
            m_lastPoint = point;
            m_stillSince = now;
            m_requested = false;
        }

        HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
        const bool overExplorer = root && IsExplorerWindow(root);
        const bool overItems = overExplorer && IsItemsView(under);

        if (overItems && !m_requested && !AnyMouseButtonDown() && now - m_stillSince >= kHoverDelayMs)
        {
            m_requested = true;
            m_requestPoint = point;
            m_requestExplorer = root;
            m_pipeline.Submit({point, under, ++m_generation, Panel::ImageSizeFor(root)});
        }

        if (!m_panel.IsVisible())
        {
            return;
        }
        if (overExplorer)
        {
            m_leaveHideAt = 0;
        }
        else if (m_leaveHideAt == 0)
        {
            m_leaveHideAt = now + kLeaveHideMs;
        }
        const bool explorerGone = !IsWindow(m_explorer) || IsIconic(m_explorer) || !IsWindowVisible(m_explorer);
        if (explorerGone || (m_leaveHideAt && now >= m_leaveHideAt) || (m_emptyHideAt && now >= m_emptyHideAt))
        {
            HidePanel();
        }
    }

    void OnResolved(UINT generation, const std::wstring& path)
    {
        if (generation != m_generation)
        {
            return; // stale
        }
        if (path.empty())
        {
            if (m_panel.IsVisible() && m_emptyHideAt == 0)
            {
                m_emptyHideAt = GetTickCount64() + kEmptyHideMs;
            }
            return;
        }
        m_emptyHideAt = 0;
        m_explorer = m_requestExplorer;
        if (m_panel.IsVisible() && m_panel.Path() == path)
        {
            return;
        }
        m_pendingPath = path;
        SetTimer(m_window, kLoadingTimer, kLoadingDelayMs, nullptr);
    }

    void OnRendered(UINT generation, const std::shared_ptr<PreviewData>& data)
    {
        if (generation != m_generation || !data)
        {
            return;
        }
        KillTimer(m_window, kLoadingTimer);
        m_pendingPath.clear();
        m_emptyHideAt = 0;
        m_leaveHideAt = 0;
        m_panel.Show(m_explorer, m_requestPoint, data);
    }

    void HidePanel()
    {
        m_panel.Hide();
        m_leaveHideAt = 0;
        m_emptyHideAt = 0;
        m_pendingPath.clear();
        KillTimer(m_window, kLoadingTimer);
    }

    // --- tray ---------------------------------------------------------------------------------

    void AddTrayIcon()
    {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = m_window;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = WM_DP_TRAY;
        data.hIcon = m_icon;
        wcsncpy_s(data.szTip, Text().trayTip, _TRUNCATE);
        Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }

    void ShowBalloon()
    {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = m_window;
        data.uID = 1;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(data.szInfoTitle, Text().balloonTitle, _TRUNCATE);
        wcsncpy_s(data.szInfo, Text().balloonText, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void ShowMenu()
    {
        const Strings& t = Text();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, t.appName);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (m_hoverEnabled ? MF_CHECKED : 0), kMenuHover, t.menuHover);
        AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), kMenuStartup, t.menuStartup);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, t.menuExit);

        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(m_window);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, 0,
                                            m_window, nullptr);
        PostMessageW(m_window, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (command)
        {
        case kMenuHover:
            m_hoverEnabled = !m_hoverEnabled;
            SaveSettings();
            if (!m_hoverEnabled)
            {
                HidePanel();
            }
            break;
        case kMenuStartup:
            SetStartup(!IsStartupEnabled());
            break;
        case kMenuExit:
            DestroyWindow(m_window);
            break;
        default:
            break;
        }
    }

    // --- settings -----------------------------------------------------------------------------

    void LoadSettings()
    {
        DWORD value = 1;
        DWORD size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HoverEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
        m_hoverEnabled = value != 0;
    }

    void SaveSettings()
    {
        const DWORD value = m_hoverEnabled ? 1 : 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HoverEnabled", REG_DWORD, &value, sizeof(value));
    }

    static bool IsStartupEnabled()
    {
        DWORD size = 0;
        return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) ==
               ERROR_SUCCESS;
    }

    static void SetStartup(bool enabled)
    {
        if (!enabled)
        {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
            return;
        }
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        const std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    HICON m_icon = nullptr;
    UINT m_taskbarCreated = 0;
    Panel m_panel;
    Pipeline m_pipeline;
    bool m_hoverEnabled = true;

    POINT m_lastPoint{};
    ULONGLONG m_stillSince = 0;
    bool m_requested = false;
    UINT m_generation = 0;
    POINT m_requestPoint{};
    HWND m_requestExplorer = nullptr;
    HWND m_explorer = nullptr;
    std::wstring m_pendingPath;
    ULONGLONG m_leaveHideAt = 0;
    ULONGLONG m_emptyHideAt = 0;
};

int RunRenderTest(const wchar_t* file, const wchar_t* output)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto data = RenderPreviewFile(file, 400);
    std::wstring report;
    if (data->readable)
    {
        report = L"OK " + std::to_wstring(data->imageWidth) + L"x" + std::to_wstring(data->imageHeight) +
                 L" layers=" + std::to_wstring(data->layers) + L" fonts=" + std::to_wstring(data->fonts.size()) + L"\n";
    }
    else
    {
        report = L"FAIL\n";
    }
    HANDLE handle = CreateFileW(output, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, utf8.data(), size, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size() - 1), &written, nullptr);
        CloseHandle(handle);
    }
    CoUninitialize();
    return data->readable ? 0 : 1;
}
} // namespace
} // namespace dp

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 4 && wcscmp(argv[1], L"--render-test") == 0)
    {
        const int result = dp::RunRenderTest(argv[2], argv[3]);
        LocalFree(argv);
        return result;
    }
    const bool firstRun = argv && argc >= 2 && wcscmp(argv[1], L"--first-run") == 0;
    if (argv)
    {
        LocalFree(argv);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DesignPeek.App.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    {
        dp::App app;
        result = app.Run(instance, firstRun);
    }
    CoUninitialize();
    if (mutex)
    {
        CloseHandle(mutex);
    }
    return result;
}
