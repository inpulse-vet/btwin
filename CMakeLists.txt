cmake_minimum_required(VERSION 3.15)
project(BTWin C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)

# Statically link the MSVC CRT (/MT) so the DLL carries no VC++ redistributable
# dependency on older, clean Windows 10 machines. Safe here: fmt is header-only and
# cppwinrt is header-only, so nothing from vcpkg links the CRT, and the C ABI never
# passes CRT objects across the library boundary.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(fmt CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)

set(LIB_SOURCES
        src/btwin.cpp
)


set(SOURCES
        src/main.c
)

add_library("btwin" SHARED ${LIB_SOURCES})
# Export symbols when compiling the library; consumers of btwin.h import them.
target_compile_definitions(btwin PRIVATE BTWIN_BUILD)
# Target Windows 10 1703 (build 15063) — the floor set by Devices::Bluetooth::BluetoothAdapter.
# Gates the classic Win32 headers only; C++/WinRT projections are not gated by these.
target_compile_definitions(btwin PRIVATE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        NTDDI_VERSION=0x0A000003
)
target_link_libraries(btwin
        PRIVATE
        fmt::fmt-header-only
        Microsoft::CppWinRT
        RuntimeObject
        WindowsApp
)

add_executable(btwin_demo ${SOURCES})
target_link_libraries(btwin_demo
        PRIVATE
            "btwin"
)