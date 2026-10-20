@echo off
rem Configure and build IPBRS (Release) with the newest installed MSVC.
rem Usage: build.bat [test]
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found; install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo No Visual Studio installation with the C++ x64 toolset was found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" || exit /b 1
if /i "%~1"=="test" ctest --test-dir "%~dp0build" --output-on-failure || exit /b 1
echo Built build\ipbrs.exe
