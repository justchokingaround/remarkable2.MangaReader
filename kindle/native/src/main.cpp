#include "app_ui.hpp"
#include <csignal>
#include <cstdio>

static kindle::AppUI* g_app = nullptr;

void signalHandler(int sig) {
    printf("\nReceived signal %d, exiting cleanly...\n", sig);
    exit(0);
}

int main(int argc, char** argv) {
    printf("=== Starting rm2manga Native App for Kindle Oasis ===\n");

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    kindle::AppUI app;
    g_app = &app;

    if (!app.init()) {
        fprintf(stderr, "Failed to initialize rm2manga display or input drivers!\n");
        return 1;
    }

    printf("rm2manga: Running main event loop...\n");
    app.run();

    printf("rm2manga: Exiting.\n");
    return 0;
}
