#include "app/application.hpp"

namespace {

int run_application() {
    ml::Application application {};
    if (!application.initialize()) {
        return 1;
    }
    return application.run();
}

}

#ifdef __ANDROID__
extern "C" int SDL_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return run_application();
}
#else
int main() {
    return run_application();
}
#endif
