@echo off
setlocal

:: --- CONFIGURATION ---
set VCPKG_PATH=C:\_vcpkg\vcpkg\scripts\buildsystems\vcpkg.cmake
set BUILD_DIR=build
set TRIPLET=x64-windows

echo Starting build for Project: Dupic...

:: Create build directory if it doesn't exist
if not exist %BUILD_DIR% (
    mkdir %BUILD_DIR%
)

:: Configure the project
:: We removed "-G Ninja" or "-G NMake" to use the default VS Generator
echo Configuring CMake...
cmake -S . -B %BUILD_DIR% ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_PATH%" ^
    -DVCPKG_TARGET_TRIPLET=%TRIPLET%

if %ERRORLEVEL% NEQ 0 (
    echo CMake configuration failed!
    pause
    exit /b %ERRORLEVEL%
)

::  Build the project

echo Building...
cmake --build %BUILD_DIR% --config Release

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo Build Successful! 
echo Your executable and libvips DLLs are in: %BUILD_DIR%\Release