#pragma once

#include <filesystem>
#include <string_view>

namespace ml {

enum class LogLevel {
    Info,
    Warning,
    Error
};

void initialize_file_logging(const std::filesystem::path& log_directory);
void shutdown_file_logging();

void log_message(LogLevel level, std::string_view message);

// Safe low-level crash log path for Android signal handlers.
// It intentionally avoids std::string, iostream and mutex usage.
void log_native_crash_signal(int signal, const void* address) noexcept;

}
