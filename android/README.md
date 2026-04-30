# Android debug port

Первый Android target: `arm64-v8a`, Vulkan, SDL3, assets внутри APK.

Требуется Android SDK:

`C:\Users\Пользователь\AppData\Local\Android\Sdk`

Через Android Studio SDK Manager установи:

- Android NDK
- Android SDK CMake
- Android SDK Command-line Tools
- Platform Tools

Сборка debug APK:

```bat
gradlew.bat :app:assembleDebug
```

Проверка телефона коротким запуском:

```bat
"C:\Users\Пользователь\AppData\Local\Android\Sdk\platform-tools\adb.exe" devices
```

Если `adb devices` висит, перезапусти USB debugging на телефоне, подтверди RSA prompt и перезапусти adb server.
