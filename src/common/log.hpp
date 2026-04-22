#pragma once

#include <string_view>

namespace ml {

enum class LogLevel {
    Info,
    Warning,
    Error
};

void log_message(LogLevel level, std::string_view message);

}

