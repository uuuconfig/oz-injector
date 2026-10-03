# oz injector — standalone build.
#
# Deliberately has NO vcpkg / Qt dependency: the UI is plain Win32 + GDI, so
# this configures and builds with nothing but CMake and MSVC. That keeps the
# tool usable as a standalone utility alongside the OpenZen project without
# dragging in a 30-minute Qt build.
cmake_minimum_required(VERSION 3.20)
project(oz_injector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "oz injector is x64-only. Configure with -A x64, e.g.\n"
        "  cmake -S . -B build -A x64")
endif()

add_executable(oz_injector WIN32
    src/main.cpp
    src/oz_injector_core.cpp
    src/oz_injector_ui.cpp
    res/injector.rc
)

target_include_directories(oz_injector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res
)

target_link_libraries(oz_injector PRIVATE
    comctl32
    psapi
    shell32      # ShellExecute / file dialogs helpers
    comdlg32     # GetOpenFileNameW
    dwmapi      # DwmSetWindowAttribute (dark title bar on Win11)
    user32
    gdi32
    uxtheme      # SetWindowTheme, dark-mode list header
)

target_compile_definitions(oz_injector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    # Needed before <commctrl.h> so the ListView_*/SetRedraw macros exist.
    COMCTL_VERSION=0x0600
)

target_compile_options(oz_injector PRIVATE
    /W4
    /permissive-
    /Zc:__cplusplus
    /utf-8
    /MP
)

# Emit the GUI binary next to the build dir so it is easy to find.
set_target_properties(oz_injector PROPERTIES
    OUTPUT_NAME "oz_injector"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
