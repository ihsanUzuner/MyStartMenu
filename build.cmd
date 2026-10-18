@echo off
rem Builds bin\MyStartMenu.exe with the MSVC command-line tools.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj

rem The version lives in VERSION (major.minor.patch), the one place it is written; git tags use it too.
set /p VERSION=<VERSION
for /f "tokens=1-3 delims=." %%a in ("%VERSION%") do (
    set MAJOR=%%a
    set MINOR=%%b
    set PATCH=%%c
)
> obj\version.h (
    echo // Made by build.cmd from VERSION; do not edit.
    echo #pragma once
    echo #define APP_VERSION "%VERSION%"
    echo #define APP_VERSION_MAJOR %MAJOR%
    echo #define APP_VERSION_MINOR %MINOR%
    echo #define APP_VERSION_PATCH %PATCH%
)
rc /nologo /Iobj /fo obj\app.res src\app.rc || exit /b 1

cl /nologo /std:c++20 /EHsc /O2 /MT /permissive- /utf-8 /W3 /DUNICODE /D_UNICODE /DNOMINMAX /Iobj ^
   /Foobj\ src\*.cpp /Fe:bin\MyStartMenu.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest obj\app.res ^
   d3d11.lib d2d1.lib dwrite.lib dwmapi.lib windowscodecs.lib windowsapp.lib CoreMessaging.lib ^
   user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib secur32.lib advapi32.lib powrprof.lib dxguid.lib propsys.lib taskschd.lib userenv.lib
