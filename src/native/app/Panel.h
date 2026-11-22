#pragma once
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "Preview.h"

namespace dp
{
// The floating side panel: big preview + design details, docked beside the Explorer window.
// It never takes keyboard focus away from Explorer.
class Panel
{
public:
    ~Panel();
    bool Create(HINSTANCE instance);

    void ShowLoading(HWND explorer, POINT cursor, const std::wstring& path);
    void Show(HWND explorer, POINT cursor, std::shared_ptr<PreviewData> data);
    void Hide();

    bool IsVisible() const;
    HWND Window() const { return m_window; }
    const std::wstring& Path() const { return m_path; }

    // Image size (physical pixels) to request so the preview is sharp on the Explorer window's monitor.
    static int ImageSizeFor(HWND explorer);

private:
    enum class Action
    {
        None,
        Open,
        ShowInFolder,
        CopyFonts,
    };
    struct Button
    {
        RECT rect{};
        std::wstring text;
        Action action = Action::None;
    };
    struct Row
    {
        std::wstring label;
        std::wstring value;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    int S(int dip) const { return MulDiv(dip, static_cast<int>(m_dpi), 96); }
    void Present(HWND explorer, POINT cursor);
    void PrepareFonts();
    void BuildRows();
    void Layout(HDC dc);
    void Paint(HDC dc);
    void DrawButton(HDC dc, const Button& button, bool hot);
    Action HitTest(POINT client) const;
    void Execute(Action action);
    void ReleaseImage();

    HWND m_window = nullptr;
    UINT m_dpi = 96;
    UINT m_fontDpi = 0;
    HFONT m_titleFont = nullptr;
    HFONT m_bodyFont = nullptr;
    HFONT m_smallFont = nullptr;
    bool m_dark = false;

    std::wstring m_path;
    bool m_loading = false;
    std::shared_ptr<PreviewData> m_data;
    HBITMAP m_image = nullptr;

    std::vector<Row> m_rows;
    std::wstring m_subtitle;
    RECT m_imageRect{};
    int m_rowsTop = 0;
    RECT m_noteRect{};
    std::vector<Button> m_buttons;
    int m_height = 0;
    int m_hotButton = -1;
    bool m_tracking = false;
    bool m_copied = false;
};
} // namespace dp
