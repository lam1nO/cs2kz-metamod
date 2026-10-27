#pragma once
#include <string>

// Проверяем смонтированные GAME-ресурсы, а не файлы исходников. Старый вызов без fallback не меняем.
template <typename Exists>
std::string SelectHudLayout(const char *preferred, const char *fallback, Exists exists)
{
    if (!fallback || !*fallback) return preferred;
    if (exists(preferred)) return preferred;
    return exists(fallback) ? fallback : "";
}
