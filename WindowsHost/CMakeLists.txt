# iframe-windows-host. Needs MSVC (Visual Studio 2019 or later, "Desktop development with C++")
# and the Windows 10 SDK 10.0.17763 or later.
cmake_minimum_required(VERSION 3.20)
project(iframe-windows-host C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ redistributable needed

add_executable(iframe-windows-host
    src/main.cpp
    src/net.cpp
    src/capture.cpp
    src/encoder.cpp
    src/streamer.cpp
    # Protocol helpers shared with the Linux client (JSON, key table).
    ../Linux/src/json.c
    ../Linux/src/keymap.c
)
target_compile_definitions(iframe-windows-host PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_RAND_S UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(iframe-windows-host PRIVATE /utf-8 /W3 /wd4244)
target_link_libraries(iframe-windows-host PRIVATE ws2_32 iphlpapi dnsapi dxgi d3d11 shell32 ole32 mfplat mfuuid
    windowscodecs strmiids)
