# AI.md

## Что это за проект

Это кроссплатформенная voxel-игра в духе `Minecraft Legacy Console Edition`, но с расчётом на современные версии Minecraft-стиля и общий мультиплеер между разными устройствами.

Целевые платформы:

- `Windows`
- `Linux`
- `macOS`
- `iOS`
- `Android`

Главная идея: игроки должны иметь возможность играть вместе в одном мире, даже если они сидят с разных устройств. Проект не должен превращаться в Windows-only прототип.

## Направление игры

Проект создаётся как воссоздание legacy console feel, но не как точная копия старой версии.

Планируемые режимы и направления:

- выживание;
- мини-игры;
- кроссплатформенная игра с друзьями;
- P2P-сессии и P2P-серверы на текущем этапе;
- возможная поддержка модов в будущем.

Сеть пока мыслится как `P2P`. Выделенные серверы, matchmaking, NAT traversal и host migration нужно проектировать аккуратно и отдельно, а не добавлять случайно в gameplay-код.

## Текущий технический стек

- `C++20`
- `CMake`
- `SDL3` для окна, ввода и платформенного слоя
- `Vulkan` как основной renderer
- `MoltenVK` предполагается для `macOS/iOS`
- `Android NDK` предполагается для Android

Renderer должен оставаться `Vulkan-first`. Если добавляется код под macOS/iOS, не нужно сразу писать отдельный Metal renderer без явной причины.

## Структура проекта

- `src/app` — главный runtime flow: инициализация, игровой цикл, связь ввода, мира и renderer.
- `src/platform` — окно, ввод, SDL3, платформенные функции.
- `src/render` — Vulkan renderer, chunk rendering, HUD, crosshair, hotbar, debug outlines.
- `src/game` — блоки, чанки, генерация мира, стриминг чанков, игрок, физика, raycast, взаимодействие с блоками.
- `assets` — текстуры, шейдеры, UI и будущие игровые ресурсы.
- `docs` — планы и документация проекта.
- `external` — внешние зависимости или заметки по ним.

## Архитектура и куда лезть

Главный поток выполнения идёт так:

1. `src/main.cpp` создаёт `Application`.
2. `src/app/application.cpp` инициализирует platform, renderer, player и world streamer.
3. Каждый кадр `Application::run()` читает input, обновляет игрока/камеру, обновляет мир, отправляет новые chunk mesh в renderer и вызывает отрисовку.
4. `Renderer` рисует мир, debug overlays, hotbar и crosshair.

Практическая навигация:

- Если нужно поменять игровой цикл, порядок обновления систем, hotbar state или связать несколько подсистем — идти в `src/app/application.cpp`.
- Если нужно добавить клавиши, мышь, gamepad, touch input или platform events — идти в `src/platform/input.hpp` и `src/platform/platform_app.cpp`.
- Если нужно поменять Vulkan, pipeline, swapchain, загрузку текстур, HUD, hotbar, crosshair, wireframe или debug outlines — идти в `src/render/renderer.hpp` и `src/render/renderer.cpp`.
- Если нужно добавить или изменить блоки, их свойства, текстуры, solid/opaque/replaceable правила — идти в `src/game/block.hpp` и `src/game/block.cpp`.
- Если нужно менять генерацию мира, высоты, воду, чанки на старте или mesh faces — идти в `src/game/world_generator.cpp`.
- Если нужно менять загрузку/выгрузку чанков, rebuild mesh, raycast, `block_at_world`, `set_block_at_world` — идти в `src/game/world_streamer.hpp` и `src/game/world_streamer.cpp`.
- Если нужно менять движение игрока, gravity, jump, collision или AABB — идти в `src/game/player_controller.hpp` и `src/game/player_controller.cpp`.
- Если нужно менять debug free-fly камеру — идти в `src/game/debug_camera.hpp` и `src/game/debug_camera.cpp`.
- Если нужно менять общие типы мира, `ChunkCoord`, `ChunkData`, `Vertex`, `BlockHit`, результаты query/set — идти в `src/game/world_types.hpp`.
- Если нужно менять GLSL — идти в `assets/shaders`.
- Если нужно менять текстуры блоков или UI — идти в `assets/textures/texture_pack/classic`.
- Если нужно менять сборку, файлы исходников, шейдеры или копирование assets — идти в `CMakeLists.txt` и `CMakePresets.json`.

Границы ответственности:

- `app` не должен знать детали Vulkan-ресурсов.
- `render` не должен принимать gameplay-решения.
- `platform` не должен знать про блоки, чанки и правила игры.
- `game` не должен напрямую зависеть от SDL, Windows API или Vulkan.
- Новую сеть лучше выносить в будущий `src/net`, а не смешивать с `WorldStreamer`.
- Сохранения мира лучше делать отдельной подсистемой, а не зашивать в генератор.

## Текущее состояние

В проекте уже есть базовый вертикальный срез:

- запуск окна через SDL3;
- Vulkan renderer;
- генерация voxel-мира по seed;
- чанки и mesh generation;
- текстуры блоков;
- игрок с базовым движением;
- ломание и постановка блоков;
- прицел и выделение блока;
- визуальный hotbar;
- debug wireframe и контуры чанков.

Некоторые системы ещё прототипные. Не считать текущую реализацию финальной архитектурой для сети, сохранений, модов и UI.

## Правила для будущего AI

- Не ломать кроссплатформенность ради быстрого Windows-only решения.
- Не завязывать gameplay-код на конкретную платформу.
- Сохранять разделение `app`, `platform`, `render`, `game`.
- Учитывать будущие P2P, мини-игры и моды при проектировании новых систем.
- Не удалять существующие игровые системы без явной причины.
- Документацию писать в `UTF-8`.
- Если меняется публичное поведение игры, фиксировать это в понятном документе или комментарии к задаче.

## Ближайшие крупные направления

- стабилизировать renderer и swapchain lifecycle;
- улучшить player controller и collision feel;
- добавить сохранение мира;
- расширить HUD и inventory;
- подготовить портирование на `Linux`, `macOS`, `Android`, `iOS`;
- спроектировать P2P multiplayer;
- отдельно продумать mini-game framework;
- позже оценить систему модов.
