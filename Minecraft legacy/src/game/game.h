#pragma once

#include <SDL3/SDL.h>

// Небольшой фасад над верхним уровнем игры.
// main.cpp вызывает эти функции, когда platform/runtime слой уже готов.
bool StartGame(float main_scale);
void HandleGameEvent(const SDL_Event& event, SDL_Window* window, bool& request_quit);
void UpdateGame(float delta_seconds, SDL_Window* window);
void RenderGame();
bool IsGameInWorld();
void ShutdownGame();
