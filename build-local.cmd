@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "VCPKG_ROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg"
set "PATH=%~dp0build-tools\ispc-v1.31.0-windows\bin;%PATH%"
set "MU_CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%MU_CMAKE%" --preset Release-NG
if errorlevel 1 exit /b 1
"%MU_CMAKE%" --build build\Release-NG --config Release --parallel 4
exit /b %errorlevel%
