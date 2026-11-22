#pragma once
#include <windows.h>

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Preview.h"

namespace dp
{
constexpr UINT WM_DP_RESOLVED = WM_APP + 10; // wParam: generation, lParam: std::wstring* (empty = nothing to show)
constexpr UINT WM_DP_RENDERED = WM_APP + 11; // wParam: generation, lParam: std::shared_ptr<PreviewData>*

struct HoverRequest
{
    POINT point{};
    HWND itemsView = nullptr;
    UINT generation = 0;
    int imageSize = 480;
};

// Background thread: "what file is under the mouse?" then "render it" (with a small cache).
// Only the newest request matters; older ones are dropped.
class Pipeline
{
public:
    ~Pipeline();
    void Start(HWND notifyWindow);
    void Stop();
    void Submit(const HoverRequest& request);

private:
    void Run();
    std::shared_ptr<PreviewData> Render(const std::wstring& path, int imageSize);
    bool HasPending();

    HWND m_notify = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<HoverRequest> m_pending;
    bool m_stop = false;

    struct CacheEntry
    {
        std::shared_ptr<PreviewData> data;
        int imageSize = 0;
    };
    std::list<CacheEntry> m_cache; // most recent first
};
} // namespace dp
