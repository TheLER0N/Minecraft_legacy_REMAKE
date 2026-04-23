@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

:: Попытка автоматически найти VsDevCmd.bat
set "VSDEVCMD="

:: 1. Ищем через vswhere (для любой установленной версии)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools -property installationPath`) do (
        set "VSDEVCMD=%%i\Common7\Tools\VsDevCmd.bat"
    )
)

:: 2. Запасные пути, если vswhere не помог
if not exist "%VSDEVCMD%" set "VSDEVCMD=C:\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"

if not exist "%VSDEVCMD%" (
    echo [error] VsDevCmd.bat not found. Please check your Visual Studio installation.
    exit /b 1
)

echo [info] Using developer environment from: %VSDEVCMD%
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
