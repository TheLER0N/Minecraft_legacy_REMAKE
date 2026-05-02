@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ==================================================
echo [1/4] ЗАПУСК СБОРКИ ANDROID (GRADLE)...
echo ==================================================

:: Путь к gradlew в папке проекта
if exist "gradlew.bat" (
    call gradlew.bat assembleRelease
) else (
    echo [ОШИБКА] Файл gradlew.bat не найден.
    pause
    exit /b
)

if errorlevel 1 (
    echo.
    echo [ОШИБКА] Сборка Android завершилась неудачей. Апдейт отменен.
    pause
    exit /b
)

echo.
echo ==================================================
echo [2/4] ТЕКУЩИЙ СТАТУС ИЗМЕНЕНИЙ (ANDROID):
echo ==================================================
git status
echo ==================================================
echo.
echo ВНИМАНИЕ: Проверьте список файлов выше. 
echo Если там есть файлы, которые НЕ нужно отправлять, нажмите 'n'.
echo.

set /p proceed="Вы уверены, что хотите отправить ВСЕ эти изменения? (y/n): "
if /i "!proceed!" neq "y" (
    echo.
    echo [ОТМЕНА] Отправка прервана пользователем.
    pause
    exit /b
)

echo.
set /p msg="Введите описание изменений для Android (комментарий): "
if "!msg!"=="" (
    echo.
    echo [ОШИБКА] Комментарий не может быть пустым. Отмена.
    pause
    exit /b
)

echo.
echo [3/4] Добавление файлов и создание коммита...
git add .
git commit -m "[Android] !msg!"

echo [4/4] Отправка на GitHub...
git push origin release

echo.
echo ==================================================
echo ГОТОВО! Android проект собран и апдейт загружен.
echo ==================================================
pause