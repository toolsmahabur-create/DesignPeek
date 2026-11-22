#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dp
{
// Reads `count` bytes at `offset` from the file being previewed. Returns bytes read, 0 at end, -1 on error.
using ReadCallback = std::function<int32_t(int64_t offset, uint8_t* destination, int32_t count)>;

struct WorkerRequest
{
    std::wstring workerPath;
    std::wstring arguments;
    uint64_t length = 0;
    DWORD timeoutMs = 3000;
    int maxDimension = 256;
    ReadCallback read;
};

struct WorkerResponse
{
    int width = 0;
    int height = 0;
    bool hasAlpha = false;
    std::vector<uint8_t> pixels; // top-down BGRA
    std::string info;            // UTF-8 "key\tvalue\n" lines (preview mode)
};

// Runs DesignPeek.Worker.exe in a Job Object (300 MB cap), serves it file bytes on request and
// kills it after timeoutMs. S_OK = image received; S_FALSE = info only; failure HRESULT otherwise.
HRESULT RunWorker(const WorkerRequest& request, WorkerResponse& response);

// Full path of `fileName` in the same folder as `module` (nullptr = the current .exe).
std::wstring SiblingPath(HMODULE module, const wchar_t* fileName);
} // namespace dp
