#include "WorkerClient.h"

#include <cstring>
#include <memory>

#include "Protocol.h"

namespace dp
{
namespace
{
constexpr DWORD kPipeBufferSize = 1 << 20;
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
        const DWORD chunk = static_cast<DWORD>(size > (1u << 30) ? (1u << 30) : size);
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
        const DWORD chunk = static_cast<DWORD>(size > (1u << 30) ? (1u << 30) : size);
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

UniqueHandle CreateWorkerJob()
{
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
    {
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                              JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = kWorkerMemoryLimit;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    return job;
}

HRESULT StartWorker(const WorkerRequest& request, WorkerProcess& worker)
{
    if (request.workerPath.empty() || GetFileAttributesW(request.workerPath.c_str()) == INVALID_FILE_ATTRIBUTES)
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

    // Our own ends must not leak into the child, or a dead worker would not show up as a broken pipe.
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);

    // Inherit exactly these two handles and nothing else from the host process.
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

    std::wstring commandLine = L"\"" + request.workerPath + L"\" " + request.arguments;
    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    const BOOL created = CreateProcessW(request.workerPath.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags,
                                        nullptr, nullptr, &startup.StartupInfo, &info);
    const HRESULT createHr = created ? S_OK : LastErrorHr();
    DeleteProcThreadAttributeList(attributes);
    if (!created)
    {
        return createHr;
    }

    UniqueHandle thread(info.hThread);
    worker.process.reset(info.hProcess);
    worker.job = CreateWorkerJob();
    if (worker.job && !AssignProcessToJobObject(worker.job.get(), info.hProcess))
    {
        worker.job.reset(); // host job forbids nesting: the timeout still protects us
    }
    ResumeThread(info.hThread);
    worker.toWorker = std::move(parentOutHandle);
    worker.fromWorker = std::move(parentInHandle);
    return S_OK; // child-side pipe ends close here
}

HRESULT Serve(const WorkerRequest& request, WorkerProcess& worker, WorkerResponse& response)
{
    HANDLE in = worker.fromWorker.get();
    HANDLE out = worker.toWorker.get();
    std::unique_ptr<uint8_t[]> buffer;
    int32_t bufferSize = 0;

    for (;;)
    {
        uint8_t op = 0;
        if (!ReadExact(in, &op, 1))
        {
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE); // crashed, out of memory, or killed by timeout
        }
        switch (op)
        {
        case protocol::kRead:
        {
            uint8_t header[12];
            if (!ReadExact(in, header, sizeof(header)))
            {
                return E_FAIL;
            }
            int64_t offset = 0;
            int32_t count = 0;
            std::memcpy(&offset, header, 8);
            std::memcpy(&count, header + 8, 4);
            if (offset < 0 || count < 0 || count > protocol::kMaxReadRequest)
            {
                return E_FAIL;
            }
            if (count > bufferSize)
            {
                buffer = std::make_unique<uint8_t[]>(size_t(count));
                bufferSize = count;
            }
            int32_t got = count == 0 ? 0 : request.read(offset, buffer.get(), count);
            if (got > count)
            {
                got = -1;
            }
            if (!WriteExact(out, &got, sizeof(got)) || (got > 0 && !WriteExact(out, buffer.get(), size_t(got))))
            {
                return E_FAIL;
            }
            break;
        }
        case protocol::kInfo:
        {
            int32_t length = 0;
            if (!ReadExact(in, &length, 4) || length < 0 || length > protocol::kMaxInfoBytes)
            {
                return E_FAIL;
            }
            response.info.resize(size_t(length));
            if (length > 0 && !ReadExact(in, response.info.data(), size_t(length)))
            {
                return E_FAIL;
            }
            break;
        }
        case protocol::kBitmap:
        {
            int32_t header[3] = {};
            if (!ReadExact(in, header, sizeof(header)))
            {
                return E_FAIL;
            }
            const int32_t width = header[0];
            const int32_t height = header[1];
            if (width <= 0 || height <= 0 || width > request.maxDimension || height > request.maxDimension)
            {
                return E_FAIL;
            }
            response.width = width;
            response.height = height;
            response.hasAlpha = header[2] != 0;
            response.pixels.resize(size_t(width) * size_t(height) * 4);
            return ReadExact(in, response.pixels.data(), response.pixels.size()) ? S_OK : E_FAIL;
        }
        case protocol::kError:
        {
            int32_t code = 0;
            ReadExact(in, &code, sizeof(code));
            return response.info.empty() ? E_FAIL : S_FALSE;
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

HRESULT RunWorker(const WorkerRequest& request, WorkerResponse& response)
{
    if (!request.read)
    {
        return E_INVALIDARG;
    }
    WorkerProcess worker;
    HRESULT hr = StartWorker(request, worker);
    if (FAILED(hr))
    {
        return hr;
    }

    HANDLE timer = nullptr;
    if (!CreateTimerQueueTimer(&timer, nullptr, OnWorkerTimeout, worker.process.get(), request.timeoutMs, 0,
                               WT_EXECUTEONLYONCE))
    {
        TerminateProcess(worker.process.get(), 1);
        return LastErrorHr();
    }

    hr = Serve(request, worker, response);

    // INVALID_HANDLE_VALUE: wait for a running callback before the process handle is closed.
    DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
    if (WaitForSingleObject(worker.process.get(), kExitGraceMs) != WAIT_OBJECT_0)
    {
        TerminateProcess(worker.process.get(), 1);
    }
    return hr;
}

std::wstring SiblingPath(HMODULE module, const wchar_t* fileName)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
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
    path += fileName;
    return path;
}
} // namespace dp
