#pragma once

// Публичные точки входа в систему меню.
// Из внешнего кода menu модуль выглядит как чёрный ящик с init/render/shutdown.
void InitializeMenu(float main_scale);
void RenderMenu();
void ShutdownMenu();
