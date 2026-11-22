#pragma once
#include <windows.h>

extern HMODULE g_hModule;

// Keeps the DLL loaded while COM objects are alive.
void DllAddRef() noexcept;
void DllRelease() noexcept;
