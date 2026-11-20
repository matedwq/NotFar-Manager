@echo off
setlocal
pushd "%~dp0"

set "host=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "host=arm64"
set "target=%host%"
if defined VSCMD_ARG_TGT_ARCH set "target=%VSCMD_ARG_TGT_ARCH%"
if /i "%~1"=="x64" (
    set "target=x64"
    shift
)
if /i "%~1"=="arm64" (
    set "target=arm64"
    shift
)
if not "%~1"=="" if /i not "%~1"=="test" (
    echo Usage: build.cmd [arm64 or x64] [test]
    exit /b 1
)

set "out=build"
set "exe=NotFar-Manager.exe"
set "component=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
set "rcflags="
if /i "%target%"=="arm64" (
    set "out=build\arm64"
    set "exe=NotFar-Manager-ARM64.exe"
    set "component=Microsoft.VisualStudio.Component.VC.Tools.ARM64"
    set "rcflags=/d NOTFAR_ARM64"
) else if /i not "%target%"=="x64" (
    echo Target must be ARM64 or x64.
    exit /b 1
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="%target%" (
    for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires %component% -find Common7\Tools\VsDevCmd.bat`) do call "%%V" -no_logo -host_arch=%host% -arch=%target%
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="%target%" (
    echo Install Visual Studio Build Tools with the MSVC toolset for %target%.
    exit /b 1
)
if not exist "%out%" mkdir "%out%"
rc /nologo /i src %rcflags% /fo "%out%\notfar.res" src\notfar.rc || exit /b 1
set "flags=/nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00"
cl /nologo /c /O2 /MT /TC /Fo%out%\miniz.obj third_party\miniz\miniz.c || exit /b 1
cl %flags% /Fo%out%\ src\main.cpp src\files.cpp src\operations.cpp src\text_files.cpp src\file_menu.cpp src\folder_picker.cpp %out%\miniz.obj %out%\notfar.res /Fe%out%\%exe% /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:NO user32.lib gdi32.lib comctl32.lib ole32.lib advapi32.lib || exit /b 1
if /i "%~1"=="test" (
    cl %flags% /Fo%out%\ %out%\files.obj %out%\operations.obj %out%\text_files.obj %out%\miniz.obj tests\files_test.cpp /Fe%out%\files-test.exe /link ole32.lib advapi32.lib || exit /b 1
    %out%\files-test.exe || exit /b 1
)
popd
