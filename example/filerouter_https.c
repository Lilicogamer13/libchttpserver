#define HTTP_SERVER_IMPLEMENTATION
#include "src/http_server.h"

#define FILE_ROUTER_IMPLEMENTATION
#include "src/file_router.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>

file_router_t *app_router = NULL;
volatile sig_atomic_t keep_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
    if (app_router != NULL) {
        printf("\n[Signal] Ctrl+C caught! Safely shutting down the file webserver...\n");
        file_router_stop(app_router);
    }
}

int main() {
    signal(SIGINT, handle_sigint);

    printf("[App] Constructing modern file-wrapper runtime server context...\n");
    app_router = file_router_create(443);
    if (!app_router) {
        fprintf(stderr, "[Error] Failed to instantiate wrapper architecture.\n");
        return 1;
    }

    http_server_enable_https(app_router->base_server, "example/cert.pem", "example/key.pem");

    file_router_add_file_route(app_router, "/", "example/www/index.html");

    printf("[App] Spawning daemon processing core loop layers...\n");
    if (file_router_start(app_router, 1) != 0) {
        fprintf(stderr, "[Error] Couldn't activate underlying network threads.\n");
        file_router_free(app_router);
        return 1;
    }

    while (keep_running) {
        static int i = 1;
        printf("%d ", i++); 
        fflush(stdout);   
        usleep(500000); 
    }

    printf("[App] De-allocating wrapper tables and shutting down safely.\n");
    file_router_free(app_router);
    
    printf("[App] Complete clean exit sequence accomplished.\n");
    return 0;
}

