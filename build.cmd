@echo off
setlocal
pushd "%~dp0"

if not defined VSCMD_ARG_TGT_ARCH (
    for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do call "%%V"
)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo Visual Studio 2022 Build Tools with Desktop development with C++ x64 are required.
    exit /b 1
)
if not exist build mkdir build
rc /nologo /i src /fo build\notfar.res src\notfar.rc || exit /b 1
set "flags=/nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00"
cl %flags% /Fobuild\ src\main.cpp src\files.cpp build\notfar.res /Febuild\NotFar-Manager.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib comctl32.lib ole32.lib shell32.lib advapi32.lib || exit /b 1
if /i "%~1"=="test" (
    cl %flags% /Fobuild\ build\files.obj tests\files_test.cpp /Febuild\files-test.exe /link ole32.lib || exit /b 1
    build\files-test.exe || exit /b 1
)
popd
