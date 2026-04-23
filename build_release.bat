@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

:: Автоматический поиск VsDevCmd.bat
set "VSDEVCMD="

if exist "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools -property installationPath`) do (
        set "VSDEVCMD=%%i\Common7\Tools\VsDevCmd.bat"
    )
)

if not exist "%VSDEVCMD%" set "VSDEVCMD=C:\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"

if not exist "%VSDEVCMD%" (
    echo [error] VsDevCmd.bat not found.
    exit /b 1
)

echo [info] Initializing developer environment...
call "%VSDEVCMD%" -arch=x64 > nul

echo [info] Configuring Release build...
cmake --preset windows-msvc-release
if errorlevel 1 (
    echo [error] CMake configure failed.
    exit /b 1
)

echo [info] Building Release...
cmake --build --preset build-windows-msvc-release --config Release
if errorlevel 1 (
    echo [error] Build failed.
    exit /b 1
)

echo [info] Packaging release...
set "PKG_DIR=minecraft_legacy_release"
if exist "%PKG_DIR%" rd /s /q "%PKG_DIR%"
mkdir "%PKG_DIR%"

:: Копируем exe
copy "build\windows-msvc-release\Release\minecraft_legacy.exe" "%PKG_DIR%\"

:: Копируем ресурсы из папки сборки (куда их скопировал CMake после сборки)
mkdir "%PKG_DIR%\assets"
xcopy /e /y "build\windows-msvc-release\Release\assets" "%PKG_DIR%\assets\"
mkdir "%PKG_DIR%\shaders"
xcopy /e /y "build\windows-msvc-release\Release\shaders" "%PKG_DIR%\shaders\"

echo [info] Creating ZIP archive...
powershell -Command "Compress-Archive -Path '%PKG_DIR%\*' -DestinationPath 'Minecraft_Legacy_Release.zip' -Force"

echo [info] Release build completed! Archive: Minecraft_Legacy_Release.zip
exit /b 0
