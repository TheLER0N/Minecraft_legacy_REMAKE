@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"

if not exist "%VSDEVCMD%" (
    echo [error] VsDevCmd.bat not found:
    echo         %VSDEVCMD%
    exit /b 1
)

call "%VSDEVCMD%" -arch=x64
if errorlevel 1 (
    echo [error] Failed to initialize Visual Studio developer environment.
    exit /b 1
)

cmake --preset windows-msvc-debug
if errorlevel 1 (
    echo [error] CMake configure failed.
    exit /b 1
)

cmake --build --preset build-windows-msvc-debug --config Debug
if errorlevel 1 (
    echo [error] Build failed.
    exit /b 1
)

echo [info] Build completed successfully.
exit /b 0
