#include "app_ui.hpp"
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

static kindle::AppUI* g_app = nullptr;

void signalHandler(int sig) {
    printf("\nReceived signal %d, exiting cleanly...\n", sig);
    exit(0);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Prevent duplicate concurrent instances
    int lock_fd = open("/tmp/rm2manga.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr, "rm2manga is already running! Exiting duplicate process.\n");
            return 0;
        }
    }

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
