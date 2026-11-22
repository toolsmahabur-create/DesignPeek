#pragma once
#include <windows.h>
#include <objidl.h>
#include <vector>

struct WorkerBitmap
{
    int width = 0;
    int height = 0;
    bool hasAlpha = false;
    std::vector<BYTE> pixels; // top-down BGRA, stride = width * 4
};

// Starts DesignPeek.Worker.exe (next to this DLL) inside a Job Object with a memory cap,
// serves it bytes from `stream` on request, and kills it after `timeoutMs`.
HRESULT RunThumbnailWorker(IStream* stream, UINT cx, DWORD timeoutMs, WorkerBitmap& result);
