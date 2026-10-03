@echo off
setlocal
set MSVC_ROOT=C:/Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set SDK_VER=10.0.26100.0
set SDK_ROOT=C:/Program Files (x86)\Windows Kits\10
set TOOLS=%MSVC_ROOT%\bin\Hostx64\x64
set "INCLUDE=%MSVC_ROOT%\include;%SDK_ROOT%\Include\%SDK_VER%\ucrt;%SDK_ROOT%\Include\%SDK_VER%\um;%SDK_ROOT%\Include\%SDK_VER%\shared"
set "LIB=%MSVC_ROOT%\lib\x64;%SDK_ROOT%\Lib\%SDK_VER%\ucrt\x64;%SDK_ROOT%\Lib\%SDK_VER%\um\x64"
set "PATH=%TOOLS%;%PATH%"
set ABS=%CD%\bin
if not exist "%ABS%" mkdir "%ABS%"
cl /nologo /O2 /MT /std:c++17 /DUNICODE /D_UNICODE probe_host.cpp /Fe:"%ABS%\probe_host.exe" /Fo:"%ABS%\\"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /std:c++17 /LD /DUNICODE /D_UNICODE probe_dll.cpp /link /DLL /OUT:"%ABS%\probe_dll.dll" /Fo:"%ABS%\\"
if errorlevel 1 exit /b 1
echo built
exit /b 0
