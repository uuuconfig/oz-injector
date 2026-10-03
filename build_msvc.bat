@echo off
REM oz injector — direct cl.exe build.
REM
REM CMake cannot detect the compiler in this environment because the sandbox
REM blocks reg.exe (vcvars64.bat needs it). Everything else is present, so this
REM script sets INCLUDE/LIB/PATH by hand and calls the toolchain directly.
REM
REM   build_msvc.bat [Debug|Release]   (default Release)

setlocal enabledelayedexpansion

set CFG=%~1
if "%CFG%"=="" set CFG=Release

set MSVC_ROOT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set SDK_VER=10.0.26100.0
set SDK_ROOT=C:\Program Files (x86)\Windows Kits\10
set TOOLS=%MSVC_ROOT%\bin\Hostx64\x64
set OUT=build\%CFG%

if not exist "%MSVC_ROOT%" (
  echo [build] MSVC toolchain not found at %MSVC_ROOT%
  exit /b 1
)

REM --- environment -------------------------------------------------------------
set "INCLUDE=%MSVC_ROOT%\include;%SDK_ROOT%\Include\%SDK_VER%\ucrt;%SDK_ROOT%\Include\%SDK_VER%\um;%SDK_ROOT%\Include\%SDK_VER%\shared;%SDK_ROOT%\Include\%SDK_VER%\winrt"
set "LIB=%MSVC_ROOT%\lib\x64;%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64;%SDK_ROOT%\Lib\%SDK_VER%\um\x64"
set "PATH=%TOOLS%;%SDK_ROOT%\bin\%SDK_VER%\x64;%PATH%"

if /i "%CFG%"=="Debug" (
  set OPT=/Od /Zi /RTC1 /D_DEBUG
) else (
  set OPT=/O2 /Ob2 /DNDEBUG
)

if not exist "%OUT%" mkdir "%OUT%"

REM Absolute paths: rc.exe and cl.exe run with pushd'd CWDs, where relative
REM /fo and /Fo paths resolve against the wrong directory.
set ABS_OUT=%CD%\%OUT%

echo [build] config   : %CFG%
echo [build] compiler : cl.exe ^(14.44.35207^)
echo [build] output   : %ABS_OUT%\oz_injector.exe
echo.

REM --- resources ---------------------------------------------------------------
echo [build] resources
pushd res
"%SDK_ROOT%\bin\%SDK_VER%\x64\rc.exe" /nologo /fo "%ABS_OUT%\injector.res" /i "." injector.rc
if errorlevel 1 (popd & echo [build] rc failed & exit /b 1)
popd

REM --- compile -----------------------------------------------------------------
pushd src
for %%F in (main.cpp oz_injector_core.cpp oz_injector_ui.cpp) do (
  echo [build] compiling %%F
  cl.exe /nologo /c /std:c++17 /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc ^
    %OPT% /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DCOMCTL_VERSION=0x0600 ^
    /I"." /I"..\res" ^
    /Fo"%ABS_OUT%\\" %%F
  if errorlevel 1 (popd & echo [build] compile failed: %%F & exit /b 1)
)
popd

REM --- link --------------------------------------------------------------------
echo [build] linking
link.exe /nologo /OUT:"%ABS_OUT%\oz_injector.exe" /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ^
  /MACHINE:X64 ^
  "%ABS_OUT%\main.obj" "%ABS_OUT%\oz_injector_core.obj" "%ABS_OUT%\oz_injector_ui.obj" ^
  "%ABS_OUT%\injector.res" ^
  /LIBPATH:"%MSVC_ROOT%\lib\x64" ^
  /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64" ^
  /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\um\x64" ^
  comctl32.lib psapi.lib shell32.lib comdlg32.lib dwmapi.lib user32.lib gdi32.lib uxtheme.lib
if errorlevel 1 (echo [build] link failed & exit /b 1)

REM --- self test -------------------------------------------------------------
if /i "%CFG%"=="Debug" goto selftest_end
echo.
echo [test] building self test
REM Compile only selftest.cpp, then link it against the objects already built
REM above. A trailing backslash inside /Fo would escape the closing quote.
pushd src
cl.exe /nologo /c /std:c++17 /W4 /utf-8 /EHsc %OPT% /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DCOMCTL_VERSION=0x0600 ^
  /I"." /I"..\res" /Fo"%ABS_OUT%\selftest.obj" selftest.cpp
if errorlevel 1 (popd & echo [test] selftest compile failed & exit /b 1)
popd
link.exe /nologo /OUT:"%ABS_OUT%\selftest.exe" /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MACHINE:X64 ^
  "%ABS_OUT%\selftest.obj" "%ABS_OUT%\oz_injector_ui.obj" "%ABS_OUT%\oz_injector_core.obj" ^
  /LIBPATH:"%MSVC_ROOT%\lib\x64" /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64" /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\um\x64" ^
  comctl32.lib psapi.lib shell32.lib comdlg32.lib dwmapi.lib user32.lib gdi32.lib uxtheme.lib
if errorlevel 1 (echo [test] selftest link failed & exit /b 1)
echo [test] running self test
"%ABS_OUT%\selftest.exe"
if errorlevel 1 (echo [test] SELF TEST FAILED & exit /b 1)
:selftest_end

echo.
echo [build] done -^> %ABS_OUT%\oz_injector.exe
endlocal
exit /b 0
