#include "Panel.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

#include "Strings.h"

namespace dp
{
namespace
{
constexpr wchar_t kClassName[] = L"DesignPeekPanel";
constexpr int kWidthDip = 340;
constexpr int kPaddingDip = 16;
constexpr int kLabelWidthDip = 104;
constexpr int kRowHeightDip = 20;
constexpr int kMaxImageHeightDip = 300;
constexpr size_t kMaxListLines = 6;
constexpr UINT_PTR kCopiedTimer = 1;

// DWM attributes (defined here so older SDK headers are not required).
constexpr DWORD kDwmCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr int kDwmRound = 2;

struct Palette
{
    COLORREF background;
    COLORREF imageBackground;
    COLORREF text;
    COLORREF dim;
    COLORREF accent;
    COLORREF accentText;
    COLORREF button;
    COLORREF buttonHot;
    COLORREF border;
    COLORREF note;
};

constexpr Palette kLight = {RGB(249, 249, 249), RGB(236, 236, 236), RGB(26, 26, 26),   RGB(96, 96, 96),
                            RGB(0, 95, 184),    RGB(255, 255, 255), RGB(255, 255, 255), RGB(240, 240, 240),
                            RGB(222, 222, 222), RGB(157, 93, 0)};
constexpr Palette kDark = {RGB(32, 32, 32),    RGB(20, 20, 20),    RGB(255, 255, 255), RGB(170, 170, 170),
                           RGB(96, 205, 255),  RGB(0, 0, 0),       RGB(52, 52, 52),    RGB(66, 66, 66),
                           RGB(58, 58, 58),    RGB(255, 185, 0)};

bool SystemUsesDarkTheme()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

std::wstring Format(const wchar_t* format, int value)
{
    wchar_t buffer[128];
    swprintf_s(buffer, format, value);
    return buffer;
}

std::wstring FileName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FormatBytes(uint64_t bytes)
{
    wchar_t buffer[64] = {};
    StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, buffer, ARRAYSIZE(buffer));
    return buffer;
}

std::wstring FormatTime(const FILETIME& time)
{
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&time, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
    {
        return {};
    }
    wchar_t date[80] = {};
    wchar_t clock[40] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, ARRAYSIZE(date), nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, clock, ARRAYSIZE(clock));
    return std::wstring(date) + L"  " + clock;
}

HFONT MakeFont(int pixelHeight, int weight)
{
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void FillRect(HDC dc, const RECT& rect, COLORREF color)
{
    SetDCBrushColor(dc, color);
    ::FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
} // namespace

Panel::~Panel()
{
    ReleaseImage();
    for (HFONT font : {m_titleFont, m_bodyFont, m_smallFont})
    {
        if (font)
        {
            DeleteObject(font);
        }
    }
    if (m_window)
    {
        DestroyWindow(m_window);
    }
}

bool Panel::Create(HINSTANCE instance)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    m_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName, L"DesignPeek",
                               WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, this);
    if (!m_window)
    {
        return false;
    }
    const int round = kDwmRound;
    DwmSetWindowAttribute(m_window, kDwmCornerPreference, &round, sizeof(round));
    return true;
}

int Panel::ImageSizeFor(HWND explorer)
{
    const UINT dpi = explorer ? GetDpiForWindow(explorer) : 96;
    return MulDiv(kWidthDip - 2 * kPaddingDip, static_cast<int>(dpi ? dpi : 96), 96);
}

bool Panel::IsVisible() const { return m_window && IsWindowVisible(m_window); }

void Panel::ShowLoading(HWND explorer, POINT cursor, const std::wstring& path)
{
    ReleaseImage();
    m_data.reset();
    m_path = path;
    m_loading = true;
    m_copied = false;
    Present(explorer, cursor);
}

void Panel::Show(HWND explorer, POINT cursor, std::shared_ptr<PreviewData> data)
{
    ReleaseImage();
    m_data = std::move(data);
    m_path = m_data ? m_data->path : std::wstring();
    m_loading = false;
    m_copied = false;

    if (m_data && m_data->imageWidth > 0 && !m_data->pixels.empty())
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = m_data->imageWidth;
        info.bmiHeader.biHeight = -m_data->imageHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (m_image && bits)
        {
            memcpy(bits, m_data->pixels.data(), m_data->pixels.size());
        }
    }
    Present(explorer, cursor);
}

void Panel::Hide()
{
    if (m_window)
    {
        ShowWindow(m_window, SW_HIDE);
    }
    m_path.clear();
}

void Panel::ReleaseImage()
{
    if (m_image)
    {
        DeleteObject(m_image);
        m_image = nullptr;
    }
}

void Panel::PrepareFonts()
{
    if (m_fontDpi == m_dpi && m_bodyFont)
    {
        return;
    }
    for (HFONT* font : {&m_titleFont, &m_bodyFont, &m_smallFont})
    {
        if (*font)
        {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    m_titleFont = MakeFont(S(15), FW_SEMIBOLD);
    m_bodyFont = MakeFont(S(13), FW_NORMAL);
    m_smallFont = MakeFont(S(12), FW_NORMAL);
    m_fontDpi = m_dpi;
}

void Panel::Present(HWND explorer, POINT cursor)
{
    m_dpi = explorer ? GetDpiForWindow(explorer) : 96;
    if (m_dpi == 0)
    {
        m_dpi = 96;
    }
    m_dark = SystemUsesDarkTheme();
    PrepareFonts();
    BuildRows();

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    Layout(dc);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(explorer, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
    {
        GetWindowRect(explorer, &frame);
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(explorer, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;

    const int width = S(kWidthDip);
    const int height = std::min(m_height, static_cast<int>(work.bottom - work.top) - S(16));
    const int gap = S(8);
    int x;
    if (frame.right + gap + width <= work.right)
    {
        x = frame.right + gap; // room on the right of Explorer
    }
    else if (frame.left - gap - width >= work.left)
    {
        x = frame.left - gap - width; // room on the left
    }
    else
    {
        // Explorer fills the screen: float inside it, on the side away from the mouse.
        const int middle = (frame.left + frame.right) / 2;
        x = cursor.x > middle ? frame.left + S(12) : frame.right - width - S(12);
    }
    const int y = std::clamp(static_cast<int>(frame.top) + S(40), static_cast<int>(work.top) + S(8),
                             std::max(static_cast<int>(work.top) + S(8), static_cast<int>(work.bottom) - height - S(8)));

    const Palette& palette = m_dark ? kDark : kLight;
    const COLORREF border = palette.border;
    DwmSetWindowAttribute(m_window, kDwmBorderColor, &border, sizeof(border));

    SetWindowPos(m_window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(m_window, nullptr, FALSE);
}

void Panel::BuildRows()
{
    const Strings& t = Text();
    m_rows.clear();
    m_subtitle.clear();
    if (m_loading || !m_data)
    {
        m_subtitle = t.loading;
        return;
    }
    const PreviewData& d = *m_data;
    if (!d.readable)
    {
        m_subtitle = t.failed;
        return;
    }

    wchar_t buffer[128];
    m_subtitle = d.format == L"PSB" ? L"Photoshop Large Document (PSB)" : L"Photoshop (PSD)";

    swprintf_s(buffer, L"%u × %u px", d.width, d.height);
    m_rows.push_back({t.labelSize, buffer});
    if (d.dpi > 0)
    {
        swprintf_s(buffer, L"%.0f DPI", d.dpi);
        m_rows.push_back({t.labelResolution, buffer});
    }
    m_rows.push_back({t.labelColor, d.mode + L" · " + Format(t.bitFormat, d.depth)});
    m_rows.push_back({t.labelFileSize, FormatBytes(d.fileSize)});
    m_rows.push_back({t.labelModified, FormatTime(d.modified)});

    if (d.layers == 0)
    {
        m_rows.push_back({t.labelLayers, t.flattened});
    }
    else if (d.layers > 0)
    {
        std::wstring value = std::to_wstring(d.layers);
        if (d.groups > 0)
        {
            value += L" · " + Format(t.groupsFormat, d.groups);
        }
        m_rows.push_back({t.labelLayers, value});

        for (size_t i = 0; i < d.topLevel.size() && i < kMaxListLines; ++i)
        {
            const auto& [isGroup, name] = d.topLevel[i];
            m_rows.push_back({i == 0 ? t.labelTopLevel : L"", (isGroup ? std::wstring(t.groupPrefix) : L"") + name});
        }
        if (d.topLevel.size() > kMaxListLines)
        {
            m_rows.push_back({L"", Format(t.moreFormat, static_cast<int>(d.topLevel.size() - kMaxListLines))});
        }
    }

    for (size_t i = 0; i < d.fonts.size() && i < kMaxListLines; ++i)
    {
        m_rows.push_back({i == 0 ? t.labelFonts : L"", d.fonts[i]});
    }
    if (d.fonts.size() > kMaxListLines)
    {
        m_rows.push_back({L"", Format(t.moreFormat, static_cast<int>(d.fonts.size() - kMaxListLines))});
    }
}

void Panel::Layout(HDC dc)
{
    const Strings& t = Text();
    const int width = S(kWidthDip);
    const int padding = S(kPaddingDip);
    int y = padding;

    y += S(22); // title
    y += S(20); // subtitle
    y += S(10);

    const int boxWidth = width - 2 * padding;
    int boxHeight = S(180);
    if (m_data && m_data->imageWidth > 0)
    {
        boxHeight = MulDiv(boxWidth, m_data->imageHeight, m_data->imageWidth);
        boxHeight = std::clamp(boxHeight, S(96), S(kMaxImageHeightDip));
    }
    m_imageRect = {padding, y, padding + boxWidth, y + boxHeight};
    y += boxHeight + S(14);

    m_rowsTop = y;
    y += static_cast<int>(m_rows.size()) * S(kRowHeightDip);

    m_noteRect = {};
    if (m_data && m_data->readable && m_data->noComposite)
    {
        y += S(6);
        RECT measure = {padding, y, width - padding, y};
        HGDIOBJ old = SelectObject(dc, m_smallFont);
        DrawTextW(dc, t.noComposite, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        m_noteRect = {padding, y, width - padding, measure.bottom};
        y = measure.bottom;
    }

    m_buttons.clear();
    if (m_data && m_data->readable)
    {
        y += S(14);
        std::vector<std::pair<std::wstring, Action>> buttons = {{t.open, Action::Open},
                                                                {t.showInFolder, Action::ShowInFolder}};
        if (!m_data->fonts.empty())
        {
            buttons.push_back({m_copied ? t.copied : t.copyFonts, Action::CopyFonts});
        }
        const int gap = S(8);
        const int count = static_cast<int>(buttons.size());
        const int buttonWidth = (boxWidth - gap * (count - 1)) / count;
        int x = padding;
        for (auto& [text, action] : buttons)
        {
            m_buttons.push_back({{x, y, x + buttonWidth, y + S(30)}, text, action});
            x += buttonWidth + gap;
        }
        y += S(30);
    }
    m_height = y + padding;
}

void Panel::DrawButton(HDC dc, const Button& button, bool hot)
{
    const Palette& p = m_dark ? kDark : kLight;
    const bool primary = button.action == Action::Open;
    SetDCBrushColor(dc, primary ? p.accent : (hot ? p.buttonHot : p.button));
    SetDCPenColor(dc, primary ? p.accent : p.border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, button.rect.left, button.rect.top, button.rect.right, button.rect.bottom, S(8), S(8));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);

    SetTextColor(dc, primary ? p.accentText : p.text);
    SelectObject(dc, m_smallFont);
    RECT text = button.rect;
    DrawTextW(dc, button.text.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void Panel::Paint(HDC dc)
{
    const Palette& p = m_dark ? kDark : kLight;
    RECT client{};
    GetClientRect(m_window, &client);
    FillRect(dc, client, p.background);
    SetBkMode(dc, TRANSPARENT);

    const int padding = S(kPaddingDip);
    const int width = client.right;

    // Title + subtitle
    SelectObject(dc, m_titleFont);
    SetTextColor(dc, p.text);
    const std::wstring name = FileName(m_path);
    RECT title = {padding, padding, width - padding, padding + S(22)};
    DrawTextW(dc, name.c_str(), -1, &title, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);

    SelectObject(dc, m_smallFont);
    SetTextColor(dc, p.dim);
    RECT subtitle = {padding, title.bottom, width - padding, title.bottom + S(20)};
    DrawTextW(dc, m_subtitle.c_str(), -1, &subtitle, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);

    // Image
    FillRect(dc, m_imageRect, p.imageBackground);
    if (m_image && m_data)
    {
        const int boxWidth = m_imageRect.right - m_imageRect.left;
        const int boxHeight = m_imageRect.bottom - m_imageRect.top;
        const double scale = std::min(static_cast<double>(boxWidth) / m_data->imageWidth,
                                      static_cast<double>(boxHeight) / m_data->imageHeight);
        const int drawWidth = std::max(1, static_cast<int>(m_data->imageWidth * scale));
        const int drawHeight = std::max(1, static_cast<int>(m_data->imageHeight * scale));
        const int x = m_imageRect.left + (boxWidth - drawWidth) / 2;
        const int y = m_imageRect.top + (boxHeight - drawHeight) / 2;

        HDC imageDc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(imageDc, m_image);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, x, y, drawWidth, drawHeight, imageDc, 0, 0, m_data->imageWidth, m_data->imageHeight, SRCCOPY);
        SelectObject(imageDc, old);
        DeleteDC(imageDc);
    }
    else if (m_loading)
    {
        SetTextColor(dc, p.dim);
        RECT box = m_imageRect;
        DrawTextW(dc, Text().loading, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Detail rows
    SelectObject(dc, m_bodyFont);
    int y = m_rowsTop;
    const int labelWidth = S(kLabelWidthDip);
    for (const Row& row : m_rows)
    {
        RECT label = {padding, y, padding + labelWidth, y + S(kRowHeightDip)};
        RECT value = {padding + labelWidth, y, width - padding, y + S(kRowHeightDip)};
        SetTextColor(dc, p.dim);
        DrawTextW(dc, row.label.c_str(), -1, &label, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
        SetTextColor(dc, p.text);
        DrawTextW(dc, row.value.c_str(), -1, &value, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
        y += S(kRowHeightDip);
    }

    if (m_noteRect.bottom > m_noteRect.top)
    {
        SelectObject(dc, m_smallFont);
        SetTextColor(dc, p.note);
        RECT note = m_noteRect;
        DrawTextW(dc, Text().noComposite, -1, &note, DT_WORDBREAK | DT_NOPREFIX);
    }

    for (size_t i = 0; i < m_buttons.size(); ++i)
    {
        DrawButton(dc, m_buttons[i], static_cast<int>(i) == m_hotButton);
    }
}

Panel::Action Panel::HitTest(POINT client) const
{
    for (const Button& button : m_buttons)
    {
        if (PtInRect(&button.rect, client))
        {
            return button.action;
        }
    }
    if (m_image && PtInRect(&m_imageRect, client))
    {
        return Action::Open;
    }
    return Action::None;
}

void Panel::Execute(Action action)
{
    if (m_path.empty())
    {
        return;
    }
    switch (action)
    {
    case Action::Open:
        ShellExecuteW(nullptr, L"open", m_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Hide();
        break;
    case Action::ShowInFolder:
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(m_path.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            CoTaskMemFree(pidl);
        }
        Hide();
        break;
    }
    case Action::CopyFonts:
    {
        if (!m_data || m_data->fonts.empty() || !OpenClipboard(m_window))
        {
            break;
        }
        std::wstring text;
        for (const auto& font : m_data->fonts)
        {
            text += font + L"\r\n";
        }
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
        {
            memcpy(GlobalLock(memory), text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
            {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
        m_copied = true;
        for (Button& button : m_buttons)
        {
            if (button.action == Action::CopyFonts)
            {
                button.text = Text().copied;
            }
        }
        InvalidateRect(m_window, nullptr, FALSE);
        SetTimer(m_window, kCopiedTimer, 1500, nullptr);
        break;
    }
    case Action::None:
        break;
    }
}

LRESULT CALLBACK Panel::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        static_cast<Panel*>(create->lpCreateParams)->m_window = window;
    }
    auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Panel::Handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(m_window, &ps);
        RECT client{};
        GetClientRect(m_window, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(buffer, bitmap);
        Paint(buffer);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(m_window, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, m_window, 0};
            m_tracking = TrackMouseEvent(&track) != FALSE;
        }
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hot = -1;
        for (size_t i = 0; i < m_buttons.size(); ++i)
        {
            if (PtInRect(&m_buttons[i].rect, point))
            {
                hot = static_cast<int>(i);
            }
        }
        if (hot != m_hotButton)
        {
            m_hotButton = hot;
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        if (m_hotButton != -1)
        {
            m_hotButton = -1;
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(m_window, &point);
        SetCursor(LoadCursorW(nullptr, HitTest(point) != Action::None ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_LBUTTONUP:
        Execute(HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
        return 0;
    case WM_TIMER:
        if (wParam == kCopiedTimer)
        {
            KillTimer(m_window, kCopiedTimer);
            m_copied = false;
            for (Button& button : m_buttons)
            {
                if (button.action == Action::CopyFonts)
                {
                    button.text = Text().copyFonts;
                }
            }
            InvalidateRect(m_window, nullptr, FALSE);
        }
        return 0;
    case WM_DPICHANGED:
        return 0; // Present() already sized the panel for the target monitor
    default:
        return DefWindowProcW(m_window, message, wParam, lParam);
    }
}
} // namespace dp
