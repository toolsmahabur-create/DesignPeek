#include "Pipeline.h"

#include <objbase.h>

#include "Resolver.h"

namespace dp
{
namespace
{
constexpr size_t kCacheSize = 32;

template <typename T>
void PostOwned(HWND window, UINT message, WPARAM wParam, T* payload)
{
    if (!PostMessageW(window, message, wParam, reinterpret_cast<LPARAM>(payload)))
    {
        delete payload;
    }
}
} // namespace

Pipeline::~Pipeline() { Stop(); }

void Pipeline::Start(HWND notifyWindow)
{
    m_notify = notifyWindow;
    m_thread = std::thread([this] { Run(); });
}

void Pipeline::Stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void Pipeline::Submit(const HoverRequest& request)
{
    {
        std::lock_guard lock(m_mutex);
        m_pending = request;
    }
    m_wake.notify_one();
}

bool Pipeline::HasPending()
{
    std::lock_guard lock(m_mutex);
    return m_pending.has_value();
}

void Pipeline::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Resolver resolver;
        resolver.Initialize();

        for (;;)
        {
            HoverRequest request;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || m_pending.has_value(); });
                if (m_stop)
                {
                    break;
                }
                request = *m_pending;
                m_pending.reset();
            }

            std::wstring path = resolver.Resolve(request.point, request.itemsView);
            const bool found = !path.empty();
            PostOwned(m_notify, WM_DP_RESOLVED, request.generation, new std::wstring(path));
            if (!found || HasPending())
            {
                continue; // the mouse already moved on
            }

            auto data = Render(path, request.imageSize);
            PostOwned(m_notify, WM_DP_RENDERED, request.generation, new std::shared_ptr<PreviewData>(std::move(data)));
        }
    }
    CoUninitialize();
}

std::shared_ptr<PreviewData> Pipeline::Render(const std::wstring& path, int imageSize)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE;
    const uint64_t size = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;

    for (auto it = m_cache.begin(); exists && it != m_cache.end(); ++it)
    {
        const PreviewData& cached = *it->data;
        if (cached.path == path && it->imageSize == imageSize && cached.fileSize == size &&
            CompareFileTime(&cached.modified, &attributes.ftLastWriteTime) == 0)
        {
            m_cache.splice(m_cache.begin(), m_cache, it);
            return m_cache.front().data;
        }
    }

    auto data = RenderPreviewFile(path, imageSize);
    if (data->readable)
    {
        m_cache.push_front({data, imageSize});
        if (m_cache.size() > kCacheSize)
        {
            m_cache.pop_back();
        }
    }
    return data;
}
} // namespace dp
