// DesignPeek.Worker.exe — the only process that parses design files.
// Started per request by DesignPeek.Shim.dll (Explorer thumbnails) and DesignPeek.exe (hover panel),
// inside a Job Object with a memory cap, and killed on timeout.
//
//   thumb   --size N --length L     pipe mode (see common/Protocol.h)
//   preview --size N --length L     pipe mode, also sends design info
//   render-file <file> <size> <out.bmp> [thumb|preview]    developer/test mode
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>

#include "../common/Protocol.h"
#include "ByteSource.h"
#include "Reader.h"
#include "Render.h"

namespace
{
using namespace dp;

bool WriteAll(HANDLE handle, const void* data, size_t size)
{
    auto* p = static_cast<const uint8_t*>(data);
    while (size > 0)
    {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1 << 30));
        if (!WriteFile(handle, p, chunk, &written, nullptr) || written == 0)
        {
            return false;
        }
        p += written;
        size -= written;
    }
    return true;
}

bool ReadAll(HANDLE handle, void* data, size_t size)
{
    auto* p = static_cast<uint8_t*>(data);
    while (size > 0)
    {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1 << 30));
        if (!ReadFile(handle, p, chunk, &read, nullptr) || read == 0)
        {
            return false;
        }
        p += read;
        size -= read;
    }
    return true;
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// Bytes come from the host over the pipe, on demand.
class PipeSource final : public ByteSource
{
public:
    PipeSource(HANDLE in, HANDLE out, uint64_t length) : m_in(in), m_out(out), m_length(length) {}

    uint64_t Length() const override { return m_length; }

    size_t ReadAt(uint64_t offset, void* buffer, size_t count) override
    {
        if (offset >= m_length)
        {
            return 0;
        }
        count = static_cast<size_t>(
            std::min<uint64_t>({uint64_t(count), uint64_t(protocol::kMaxReadRequest), m_length - offset}));
        uint8_t request[13];
        request[0] = protocol::kRead;
        const int64_t signedOffset = static_cast<int64_t>(offset);
        const int32_t signedCount = static_cast<int32_t>(count);
        std::memcpy(request + 1, &signedOffset, 8);
        std::memcpy(request + 9, &signedCount, 4);
        int32_t received = -1;
        if (!WriteAll(m_out, request, sizeof(request)) || !ReadAll(m_in, &received, 4))
        {
            throw FormatError("host pipe closed");
        }
        if (received < 0 || received > signedCount)
        {
            throw FormatError("host could not read file");
        }
        if (received > 0 && !ReadAll(m_in, buffer, size_t(received)))
        {
            throw FormatError("host pipe closed");
        }
        return size_t(received);
    }

private:
    HANDLE m_in;
    HANDLE m_out;
    uint64_t m_length;
};

class FileSource final : public ByteSource
{
public:
    explicit FileSource(const wchar_t* path)
    {
        m_file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER size{};
        if (m_file == INVALID_HANDLE_VALUE || !GetFileSizeEx(m_file, &size))
        {
            throw FormatError("cannot open file");
        }
        m_length = static_cast<uint64_t>(size.QuadPart);
    }
    ~FileSource() override
    {
        if (m_file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_file);
        }
    }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    uint64_t Length() const override { return m_length; }

    size_t ReadAt(uint64_t offset, void* buffer, size_t count) override
    {
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!ReadFile(m_file, buffer, static_cast<DWORD>(std::min<size_t>(count, 1 << 30)), &read, &overlapped))
        {
            if (GetLastError() == ERROR_HANDLE_EOF)
            {
                return 0;
            }
            throw FormatError("read failed");
        }
        return read;
    }

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
    uint64_t m_length = 0;
};

bool SendImage(HANDLE out, const Image& image)
{
    uint8_t header[13];
    header[0] = protocol::kBitmap;
    const int32_t values[3] = {image.width, image.height, 0};
    std::memcpy(header + 1, values, sizeof(values));
    return WriteAll(out, header, sizeof(header)) && WriteAll(out, image.bgra.data(), image.bgra.size());
}

bool SendInfo(HANDLE out, const std::wstring& info)
{
    std::string utf8 = ToUtf8(info);
    if (utf8.size() > size_t(protocol::kMaxInfoBytes))
    {
        utf8.resize(size_t(protocol::kMaxInfoBytes));
    }
    uint8_t header[5];
    header[0] = protocol::kInfo;
    const int32_t length = static_cast<int32_t>(utf8.size());
    std::memcpy(header + 1, &length, 4);
    return WriteAll(out, header, sizeof(header)) && WriteAll(out, utf8.data(), utf8.size());
}

bool SendError(HANDLE out, int32_t code)
{
    uint8_t message[5];
    message[0] = protocol::kError;
    std::memcpy(message + 1, &code, 4);
    return WriteAll(out, message, sizeof(message));
}

const wchar_t* GetOption(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::wcscmp(argv[i], name) == 0)
        {
            return argv[i + 1];
        }
    }
    return nullptr;
}

int RunPipe(int argc, wchar_t** argv)
{
    const bool preview = std::wcscmp(argv[1], L"preview") == 0;
    const wchar_t* sizeText = GetOption(argc, argv, L"--size");
    const wchar_t* lengthText = GetOption(argc, argv, L"--length");
    if (!sizeText || !lengthText)
    {
        return 2;
    }
    const int size = _wtoi(sizeText);
    const uint64_t length = _wcstoui64(lengthText, nullptr, 10);
    if (size < 1 || size > protocol::kMaxImageDimension)
    {
        return 2;
    }

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    PipeSource source(in, out, length);
    Reader reader(source);
    try
    {
        Image image;
        if (preview)
        {
            std::wstring info;
            image = RenderPreview(reader, size, info);
            if (!info.empty())
            {
                SendInfo(out, info);
            }
        }
        else
        {
            image = RenderThumbnail(reader, size);
        }
        if (image.Empty())
        {
            SendError(out, 1);
            return 0;
        }
        return SendImage(out, image) ? 0 : 1;
    }
    catch (const std::exception&)
    {
        SendError(out, 2);
        return 1;
    }
}

void Print(const std::wstring& text)
{
    const std::string utf8 = ToUtf8(text);
    WriteAll(GetStdHandle(STD_OUTPUT_HANDLE), utf8.data(), utf8.size());
}

bool WriteBmp(const wchar_t* path, const Image& image)
{
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = image.width;
    info.biHeight = -image.height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = static_cast<DWORD>(file.bfOffBits + image.bgra.size());

    HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const bool ok = WriteAll(handle, &file, sizeof(file)) && WriteAll(handle, &info, sizeof(info)) &&
                    WriteAll(handle, image.bgra.data(), image.bgra.size());
    CloseHandle(handle);
    return ok;
}

int RunRenderFile(const wchar_t* path, int size, const wchar_t* output, bool preview)
{
    if (size < 1 || size > protocol::kMaxImageDimension)
    {
        return 2;
    }
    try
    {
        FileSource source(path);
        Reader reader(source);
        std::wstring info;
        const Image image = preview ? RenderPreview(reader, size, info) : RenderThumbnail(reader, size);
        if (image.Empty())
        {
            Print(L"FAIL\n" + info);
            return 1;
        }
        if (!WriteBmp(output, image))
        {
            Print(L"FAIL cannot write output\n");
            return 1;
        }
        Print(L"OK " + std::to_wstring(image.width) + L"x" + std::to_wstring(image.height) + L"\n" + info);
        return 0;
    }
    catch (const std::exception&)
    {
        Print(L"FAIL damaged file\n");
        return 1;
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        return 3;
    }

    int result = 2;
    try
    {
        if (argc >= 2 && (std::wcscmp(argv[1], L"thumb") == 0 || std::wcscmp(argv[1], L"preview") == 0))
        {
            result = RunPipe(argc, argv);
        }
        else if (argc >= 5 && std::wcscmp(argv[1], L"render-file") == 0)
        {
            const bool preview = argc >= 6 && std::wcscmp(argv[5], L"preview") == 0;
            result = RunRenderFile(argv[2], _wtoi(argv[3]), argv[4], preview);
        }
        else
        {
            Print(L"DesignPeek worker. Started automatically by Windows Explorer and DesignPeek.\n");
        }
    }
    catch (...)
    {
        result = 1;
    }
    CoUninitialize();
    return result;
}
