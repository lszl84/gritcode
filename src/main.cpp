#include "app.h"
#include <cstdio>

int main() {
    App app;
    if (!app.Init()) {
        fprintf(stderr, "Failed to initialize app\n");
        return 1;
    }
    app.Run();
    return 0;
}
