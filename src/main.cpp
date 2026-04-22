#include "app/application.hpp"

int main() {
    ml::Application application {};
    if (!application.initialize()) {
        return 1;
    }
    return application.run();
}
