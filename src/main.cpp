#include "app/application.hpp"
#include "common/log.hpp"

#include <exception>
#include <stdexcept>
#include <string>

#ifdef __ANDROID__
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

namespace {

#ifdef __ANDROID__


void android_signal_handler(int signal, siginfo_t* info, void* context) {
    (void)context;

    const void* address = info != nullptr ? info->si_addr : nullptr;
    ml::log_native_crash_signal(signal, address);

    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(signal, &default_action, nullptr);
    raise(signal);

    std::_Exit(128 + signal);
}

void install_android_crash_handlers() {
    struct sigaction action {};
    action.sa_sigaction = android_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;

    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);

    std::set_terminate([]() {
        try {
            std::exception_ptr exception = std::current_exception();
            if (exception != nullptr) {
                std::rethrow_exception(exception);
            }
            ml::log_message(ml::LogLevel::Error, "std::terminate called without active exception");
        } catch (const std::exception& error) {
            ml::log_message(
                ml::LogLevel::Error,
                std::string("std::terminate called after exception: ") + error.what()
            );
        } catch (...) {
            ml::log_message(ml::LogLevel::Error, "std::terminate called after unknown exception");
        }

        std::abort();
    });

    ml::log_message(ml::LogLevel::Info, "Android crash handlers installed");
}

#endif

int run_application() {
    try {
        ml::Application application {};
        if (!application.initialize()) {
            ml::log_message(ml::LogLevel::Error, "Application initialization failed");
            return 1;
        }

        return application.run();
    } catch (const std::exception& error) {
        ml::log_message(
            ml::LogLevel::Error,
            std::string("Unhandled C++ exception: ") + error.what()
        );
        return 1;
    } catch (...) {
        ml::log_message(ml::LogLevel::Error, "Unhandled unknown C++ exception");
        return 1;
    }
}

}

#ifdef __ANDROID__
extern "C" int SDL_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    install_android_crash_handlers();
    return run_application();
}
#else
int main() {
    return run_application();
}
#endif