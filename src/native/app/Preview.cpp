#include "Preview.h"

#include <algorithm>
#include <cwctype>

#include "../common/WorkerClient.h"

namespace dp
{
namespace
{
constexpr DWORD kPreviewTimeoutMs = 8000;

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void ParseInfo(const std::string& utf8, PreviewData& data)
{
    const std::wstring text = FromUtf8(utf8);
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
        {
            end = text.size();
        }
        const std::wstring line = text.substr(start, end - start);
        start = end + 1;

        const size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos)
        {
            continue;
        }
        const std::wstring key = line.substr(0, tab);
        const std::wstring value = line.substr(tab + 1);
        if (key == L"format") data.format = value;
        else if (key == L"width") data.width = static_cast<uint32_t>(_wtoi(value.c_str()));
        else if (key == L"height") data.height = static_cast<uint32_t>(_wtoi(value.c_str()));
        else if (key == L"dpi") data.dpi = _wtof(value.c_str());
        else if (key == L"mode") data.mode = value;
        else if (key == L"depth") data.depth = _wtoi(value.c_str());
        else if (key == L"layers") data.layers = _wtoi(value.c_str());
        else if (key == L"groups") data.groups = _wtoi(value.c_str());
        else if (key == L"group") data.topLevel.emplace_back(true, value);
        else if (key == L"layer") data.topLevel.emplace_back(false, value);
        else if (key == L"font") data.fonts.push_back(value);
        else if (key == L"note" && value == L"no-composite") data.noComposite = true;
    }
}
} // namespace

bool IsSupportedFile(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
    {
        return false;
    }
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".psd" || ext == L".psb";
}

std::shared_ptr<PreviewData> RenderPreviewFile(const std::wstring& path, int imageSize)
{
    auto data = std::make_shared<PreviewData>();
    data->path = path;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return data;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    GetFileTime(file, nullptr, nullptr, &data->modified);
    data->fileSize = static_cast<uint64_t>(size.QuadPart);

    if (data->fileSize >= 4)
    {
        WorkerRequest request;
        request.workerPath = SiblingPath(nullptr, L"DesignPeek.Worker.exe");
        request.arguments =
            L"preview --size " + std::to_wstring(imageSize) + L" --length " + std::to_wstring(data->fileSize);
        request.length = data->fileSize;
        request.timeoutMs = kPreviewTimeoutMs;
        request.maxDimension = imageSize;
        request.read = [file](int64_t offset, uint8_t* destination, int32_t count) -> int32_t {
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(offset);
            overlapped.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
            DWORD read = 0;
            if (!ReadFile(file, destination, static_cast<DWORD>(count), &read, &overlapped))
            {
                return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
            }
            return static_cast<int32_t>(read);
        };

        WorkerResponse response;
        const HRESULT hr = RunWorker(request, response);
        if (SUCCEEDED(hr))
        {
            ParseInfo(response.info, *data);
            data->readable = !data->format.empty();
            if (hr == S_OK)
            {
                data->imageWidth = response.width;
                data->imageHeight = response.height;
                data->pixels = std::move(response.pixels);
            }
        }
    }
    CloseHandle(file);
    return data;
}
} // namespace dp
