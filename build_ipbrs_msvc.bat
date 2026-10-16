@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b %errorlevel%
if not exist build mkdir build
cl /nologo /std:c++14 /EHsc /openmp /W4 /Iinclude src\scheduler.cpp src\backup.cpp src\recovery.cpp src\win_crypto.cpp src\backup_main.cpp Advapi32.lib /Fe:build\ipbrs-backup.exe
if errorlevel 1 exit /b %errorlevel%
echo Built build\ipbrs-backup.exe
