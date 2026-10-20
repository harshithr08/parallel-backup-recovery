@echo off
setlocal
rem Locate any installed Visual Studio (2019 or later) that has the C++ toolset.
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
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b %errorlevel%
if not exist build mkdir build
cl /nologo /std:c++14 /EHsc /O2 /openmp /W4 /utf-8 /Iinclude src\scheduler.cpp src\backup.cpp src\recovery.cpp src\win_crypto.cpp src\backup_main.cpp Advapi32.lib /Fe:build\ipbrs-backup.exe
if errorlevel 1 exit /b %errorlevel%
echo Built build\ipbrs-backup.exe
