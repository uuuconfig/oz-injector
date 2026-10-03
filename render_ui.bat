@echo off
REM Renders the UI to %TEMP%\oz_injector_ui.png from inside the process, then
REM copies it next to the exe. An external enumerator cannot see this sandbox's
REM GUI session, so the pixels have to be grabbed by the process that owns them.
setlocal
set MSVC_ROOT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set SDK_VER=10.0.26100.0
set SDK_ROOT=C:\Program Files (x86)\Windows Kits\10
set TOOLS=%MSVC_ROOT%\bin\Hostx64\x64
set "INCLUDE=%MSVC_ROOT%\include;%SDK_ROOT%\Include\%SDK_VER%\ucrt;%SDK_ROOT%\Include\%SDK_VER%\um;%SDK_ROOT%\Include\%SDK_VER%\shared"
set "LIB=%MSVC_ROOT%\lib\x64;%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64;%SDK_ROOT%\Lib\%SDK_VER%\um\x64"
set "PATH=%TOOLS%;%SDK_ROOT%\bin\%SDK_VER%\x64;%PATH%"
set ABS=%CD%\build\Release
if not exist "%ABS%" mkdir "%ABS%"
pushd src
cl /nologo /c /std:c++17 /W4 /utf-8 /EHsc /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DCOMCTL_VERSION=0x0600 ^
  /I"." /I"..\res" /Fo"%ABS%\render.obj" render.cpp
if errorlevel 1 (popd & echo render compile failed & exit /b 1)
popd
link /nologo /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MACHINE:X64 /OUT:"%ABS%\render.exe" ^
  "%ABS%\render.obj" "%ABS%\oz_injector_ui.obj" "%ABS%\oz_injector_core.obj" ^
  /LIBPATH:"%MSVC_ROOT%\lib\x64" /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64" /LIBPATH:"%SDK_ROOT%\Lib\%SDK_VER%\um\x64" ^
  comctl32.lib psapi.lib shell32.lib comdlg32.lib dwmapi.lib user32.lib gdi32.lib uxtheme.lib
if errorlevel 1 (echo render link failed & exit /b 1)
"%ABS%\render.exe"
if errorlevel 1 (echo render failed & exit /b 1)
echo rendered -^> %TEMP%\oz_injector_ui.png
exit /b 0
