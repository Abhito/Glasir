@echo off
rem SPDX-License-Identifier: MIT
rem Builds Glasir, runs the tests and puts the portable app in out\Glasir.
rem   build.cmd           Release build with Visual Studio 2026
rem   build.cmd vs2022    Release build with Visual Studio 2022
setlocal
cd /d "%~dp0"

set "CONFIGURE=vs"
set "BUILD=release"
if /i "%~1"=="vs2022" (
    set "CONFIGURE=vs2022"
    set "BUILD=release-vs2022"
)

where cmake >nul 2>&1 || (
    echo CMake was not found. Install it from https://cmake.org or with Visual Studio.
    exit /b 1
)

cmake --preset %CONFIGURE% || exit /b 1
cmake --build --preset %BUILD% || exit /b 1
ctest --preset %BUILD% || exit /b 1
cmake --install build\%CONFIGURE% --config Release || exit /b 1

echo.
echo Built: %~dp0out\Glasir
