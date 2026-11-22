#include "WorkerHost.h"

#include <cstring>
#include <memory>
#include <string>

#include "Module.h"

namespace
{
constexpr wchar_t kWorkerExe[] = L"DesignPeek.Worker.exe";
constexpr DWORD kPipeBufferSize = 1 << 20;
constexpr INT32 kMaxReadRequest = 8 * 1024 * 1024;
constexpr SIZE_T kWorkerMemoryLimit = 300ull * 1024 * 1024;
constexpr DWORD kExitGraceMs = 1000;

struct HandleCloser
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct WorkerProcess
{
    UniqueHandle process;
    UniqueHandle job;
    UniqueHandle toWorker;
    UniqueHandle fromWorker;
};

HRESULT LastErrorHr()
{
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}

bool ReadExact(HANDLE pipe, void* buffer, size_t size)
{
    auto* cursor = static_cast<BYTE*>(buffer);
    while (size > 0)
    {
        const DWORD chunk = static_cast<DWORD>(size > MAXDWORD ? MAXDWORD : size);
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || read == 0)
        {
            return false;
        }
        cursor += read;
        size -= read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* buffer, size_t size)
{
    const auto* cursor = static_cast<const BYTE*>(buffer);
    while (size > 0)
    {
        const DWORD chunk = static_cast<DWORD>(size > MAXDWORD ? MAXDWORD : size);
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, chunk, &written, nullptr) || written == 0)
        {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

std::wstring GetWorkerPath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(g_hModule, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < path.size())
        {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return {};
    }
    path.resize(slash + 1);
    path += kWorkerExe;
    return path;
}

UniqueHandle CreateWorkerJob()
{
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
    {
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = kWorkerMemoryLimit;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    return job;
}

HRESULT StartWorker(const std::wstring& arguments, WorkerProcess& worker)
{
    const std::wstring exe = GetWorkerPath();
    if (exe.empty())
    {
        return E_FAIL;
    }
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};

    HANDLE childIn = nullptr;
    HANDLE parentOut = nullptr;
    if (!CreatePipe(&childIn, &parentOut, &inheritable, kPipeBufferSize))
    {
        return LastErrorHr();
    }
    UniqueHandle childInHandle(childIn);
    UniqueHandle parentOutHandle(parentOut);

    HANDLE parentIn = nullptr;
    HANDLE childOut = nullptr;
    if (!CreatePipe(&parentIn, &childOut, &inheritable, kPipeBufferSize))
    {
        return LastErrorHr();
    }
    UniqueHandle parentInHandle(parentIn);
    UniqueHandle childOutHandle(childOut);

    // Our own ends must not leak into the child, or broken-pipe detection stops working.
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);

    // Inherit exactly these two handles, nothing else from the host process.
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    auto attributeBuffer = std::make_unique<BYTE[]>(attributeSize);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.get());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
    {
        return LastErrorHr();
    }
    HANDLE inherited[2] = {childIn, childOut};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr))
    {
        const HRESULT hr = LastErrorHr();
        DeleteProcThreadAttributeList(attributes);
        return hr;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childIn;
    startup.StartupInfo.hStdOutput = childOut;
    startup.StartupInfo.hStdError = nullptr;
    startup.lpAttributeList = attributes;

    std::wstring commandLine = L"\"" + exe + L"\" " + arguments;
    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    const BOOL created = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                        nullptr, &startup.StartupInfo, &info);
    const HRESULT createHr = created ? S_OK : LastErrorHr();
    DeleteProcThreadAttributeList(attributes);
    if (!created)
    {
        return createHr;
    }

    UniqueHandle thread(info.hThread);
    worker.process.reset(info.hProcess);

    // If the host already runs inside a job that forbids nesting, we still have the timeout.
    worker.job = CreateWorkerJob();
    if (worker.job && !AssignProcessToJobObject(worker.job.get(), info.hProcess))
    {
        worker.job.reset();
    }

    ResumeThread(info.hThread);
    worker.toWorker = std::move(parentOutHandle);
    worker.fromWorker = std::move(parentInHandle);
    return S_OK; // child-side pipe ends close here, so a dead worker shows up as a broken pipe
}

INT32 ReadFromStream(IStream* stream, INT64 offset, BYTE* destination, INT32 count)
{
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr)))
    {
        return -1;
    }
    INT32 total = 0;
    while (total < count)
    {
        ULONG read = 0;
        const HRESULT hr = stream->Read(destination + total, static_cast<ULONG>(count - total), &read);
        if (FAILED(hr))
        {
            return total > 0 ? total : -1;
        }
        total += static_cast<INT32>(read);
        if (hr == S_FALSE || read == 0)
        {
            break;
        }
    }
    return total;
}

HRESULT ServeWorker(IStream* stream, UINT cx, WorkerProcess& worker, WorkerBitmap& result)
{
    HANDLE in = worker.fromWorker.get();
    HANDLE out = worker.toWorker.get();
    std::unique_ptr<BYTE[]> buffer;
    INT32 bufferSize = 0;

    for (;;)
    {
        BYTE op = 0;
        if (!ReadExact(in, &op, 1))
        {
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); // crashed, killed by timeout, or out of memory
        }

        switch (op)
        {
        case 'R':
        {
            BYTE header[12];
            if (!ReadExact(in, header, sizeof(header)))
            {
                return E_FAIL;
            }
            INT64 offset = 0;
            INT32 count = 0;
            std::memcpy(&offset, header, sizeof(offset));
            std::memcpy(&count, header + 8, sizeof(count));
            if (offset < 0 || count < 0 || count > kMaxReadRequest)
            {
                return E_FAIL;
            }
            if (count > bufferSize)
            {
                buffer = std::make_unique<BYTE[]>(count);
                bufferSize = count;
            }
            const INT32 got = count == 0 ? 0 : ReadFromStream(stream, offset, buffer.get(), count);
            if (!WriteExact(out, &got, sizeof(got)) || (got > 0 && !WriteExact(out, buffer.get(), got)))
            {
                return E_FAIL;
            }
            break;
        }
        case 'B':
        {
            INT32 header[3] = {};
            if (!ReadExact(in, header, sizeof(header)))
            {
                return E_FAIL;
            }
            const INT32 width = header[0];
            const INT32 height = header[1];
            if (width <= 0 || height <= 0 || static_cast<UINT>(width) > cx || static_cast<UINT>(height) > cx)
            {
                return E_FAIL;
            }
            result.width = width;
            result.height = height;
            result.hasAlpha = header[2] != 0;
            result.pixels.resize(static_cast<size_t>(width) * height * 4);
            return ReadExact(in, result.pixels.data(), result.pixels.size()) ? S_OK : E_FAIL;
        }
        case 'E':
        {
            INT32 code = 0;
            ReadExact(in, &code, sizeof(code));
            return E_FAIL;
        }
        default:
            return E_FAIL;
        }
    }
}

VOID CALLBACK OnWorkerTimeout(PVOID process, BOOLEAN)
{
    TerminateProcess(static_cast<HANDLE>(process), ERROR_TIMEOUT);
}
} // namespace

HRESULT RunThumbnailWorker(IStream* stream, UINT cx, DWORD timeoutMs, WorkerBitmap& result)
{
    STATSTG stat{};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr))
    {
        return hr;
    }
    const ULONGLONG length = stat.cbSize.QuadPart;
    if (length < 4)
    {
        return E_FAIL; // zero-byte / empty file: keep the normal icon
    }

    const std::wstring arguments =
        L"thumb --size " + std::to_wstring(cx) + L" --length " + std::to_wstring(length);

    WorkerProcess worker;
    hr = StartWorker(arguments, worker);
    if (FAILED(hr))
    {
        return hr;
    }

    HANDLE timer = nullptr;
    if (!CreateTimerQueueTimer(&timer, nullptr, OnWorkerTimeout, worker.process.get(), timeoutMs, 0,
                               WT_EXECUTEONLYONCE))
    {
        TerminateProcess(worker.process.get(), 1);
        return LastErrorHr();
    }

    hr = ServeWorker(stream, cx, worker, result);

    // INVALID_HANDLE_VALUE: wait for a running callback to finish before the process handle closes.
    DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
    if (WaitForSingleObject(worker.process.get(), kExitGraceMs) != WAIT_OBJECT_0)
    {
        TerminateProcess(worker.process.get(), 1);
    }
    return hr;
}
