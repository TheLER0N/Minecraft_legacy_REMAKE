# Minecraft Legacy Remake

Minecraft Legacy Remake is a fan-made C++/Vulkan prototype inspired by the Minecraft Legacy Console Edition style. The project focuses on a console-like main menu, classic block interaction, chunk streaming, atmospheric music, and fullscreen desktop play.

## Features

- Legacy Console Edition-style menu with animated panorama, startup splash screens, button sounds, and background music.
- Procedural block world with chunk generation, greedy meshing, lighting, water, trees, and basic terrain editing.
- Optimized chunk rebuild snapshots and upload prioritization for smoother streaming.
- Render distance controls with `Ctrl+F7` and `Ctrl+F8`.
- Keyboard, mouse, and Xbox-style gamepad input.
- Borderless fullscreen startup with `F11` / `Alt+Enter` toggle.
- OGG background music playback from `assets/sound/music/game/unused`.

## Controls

- `WASD`: move
- Mouse: look around
- Left mouse / `RT`: break block
- Right mouse / `LT`: place block
- `Space` / `A`: jump or skip startup splash
- Number keys / `LB` / `RB`: hotbar selection
- `F11` or `Alt+Enter`: toggle fullscreen
- `Ctrl+F7` / `Ctrl+F8`: decrease/increase render distance
- `F3`: debug HUD
- `F4`: leaves render mode
- `Start` on gamepad: confirm menu item or toggle mouse capture in world

## Build

Use the Windows build script:

```bat
build.bat
```

The debug executable is produced at:

```text
build/windows-msvc-debug/Debug/minecraft_legacy.exe
```

## Notes

This is a work-in-progress remake prototype, not an official Minecraft product. Minecraft is owned by Mojang/Microsoft.
