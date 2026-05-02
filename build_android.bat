@echo off
chcp 65001 >nul
setlocal

:: Переходим в директорию, где лежит этот батник
cd /d "%~dp0"

echo ==================================================
echo ЗАПУСК СБОРКИ ANDROID (Release APK)...
echo ==================================================

:: Проверка наличия gradlew
if not exist "gradlew.bat" (
    echo [ОШИБКА] Файл gradlew.bat не найден в корневой директории.
    pause
    exit /b 1
)

echo [info] Running Gradle build...
call gradlew.bat assembleRelease

if errorlevel 1 (
    echo.
    echo [ОШИБКА] Сборка Android завершилась неудачей.
    pause
    exit /b 1
)

echo.
echo ==================================================
echo СБОРКА ЗАВЕРШЕНА УСПЕШНО!
echo.
echo APK файл можно найти по пути:
echo android\app\build\outputs\apk\release\app-release.apk
echo ==================================================
pause