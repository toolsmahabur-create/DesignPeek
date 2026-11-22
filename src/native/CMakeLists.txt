cmake_minimum_required(VERSION 3.20)
project(DesignPeek LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static C runtime: nothing extra to install on users' PCs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /guard:cf /EHsc /O2 /GL)
add_link_options(/GUARD:CF /LTCG /OPT:REF /OPT:ICF)

# Shared host-side code: start the sandboxed worker and serve it file bytes.
add_library(dp_common STATIC common/WorkerClient.cpp)

# DesignPeek.Worker.exe: the only process that parses design files.
add_executable(DesignPeek.Worker
    worker/main.cpp
    worker/Reader.cpp
    worker/Psd.cpp
    worker/Render.cpp
    worker/Wic.cpp
)
target_link_libraries(DesignPeek.Worker PRIVATE ole32 windowscodecs)

# DesignPeek.Shim.dll: Explorer thumbnail provider (COM). Never parses files itself.
add_library(DesignPeek.Shim SHARED
    shim/dllmain.cpp
    shim/ThumbnailProvider.cpp
    shim/DesignPeek.Shim.def
)
target_link_libraries(DesignPeek.Shim PRIVATE dp_common ole32 uuid)

# DesignPeek.exe: tray app with the hover side panel.
add_executable(DesignPeek WIN32
    app/main.cpp
    app/AppIcon.cpp
    app/Panel.cpp
    app/Pipeline.cpp
    app/Preview.cpp
    app/Resolver.cpp
    app/Strings.cpp
)
target_link_libraries(DesignPeek PRIVATE dp_common ole32 oleaut32 uuid shell32 shlwapi dwmapi user32 gdi32 advapi32)

# CI harness: loads the shim exactly like Explorer does (no registry needed).
add_executable(DesignPeek.ShimTest shimtest/main.cpp)
target_include_directories(DesignPeek.ShimTest PRIVATE shim)
target_link_libraries(DesignPeek.ShimTest PRIVATE ole32 shlwapi uuid)
