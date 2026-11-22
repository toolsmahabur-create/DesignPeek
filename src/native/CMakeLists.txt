cmake_minimum_required(VERSION 3.20)
project(DesignPeekNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static C runtime: users don't need the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX)
add_compile_options(/W4 /permissive- /utf-8 /guard:cf /EHsc)
add_link_options(/GUARD:CF)

# The tiny COM DLL that Windows loads. It never parses files itself: it starts
# DesignPeek.Worker.exe in a locked-down child process and streams bytes to it.
add_library(DesignPeek.Shim SHARED
    shim/dllmain.cpp
    shim/ThumbnailProvider.cpp
    shim/WorkerHost.cpp
    shim/DesignPeek.Shim.def
)
target_link_libraries(DesignPeek.Shim PRIVATE ole32 uuid)

# Test harness used by CI: loads the shim exactly like Explorer does (without registry).
add_executable(DesignPeek.ShimTest shimtest/main.cpp)
target_include_directories(DesignPeek.ShimTest PRIVATE shim)
target_link_libraries(DesignPeek.ShimTest PRIVATE ole32 shlwapi uuid)
