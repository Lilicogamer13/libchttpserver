#include "src/http_server.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

http_server_t *server = NULL;
volatile sig_atomic_t keep_running = 1;

void handle_sigint(int sig) {
    (void)sig; // Suppress compiler warnings for unused parameter
    keep_running = 0;
    
    if (server != NULL) {
        printf("\n[Signal] Ctrl+C captured. Stopping server thread context...\n");
        http_server_stop(server); 
    }
}

// WARN: Anything including the stuff to be hosted needs to be set before main
void handle_home_page(const http_request_t *req, int client_fd, SSL *ssl) {
    (void)req; // Suppress compiler warnings for unused parameter
    const char *body = 
        "<html>"
        "<body>"
        "<h1>Hello, World!</h1>"
        "<p>The World is Always a Great Thing to Say Hello to!</p>"
        "</body>"
        "</html>";
    http_send_response(client_fd, ssl, 200, "OK", "text/html", body);
}

int main() {
    signal(SIGINT, handle_sigint);

    printf("[App] Creating server instance...\n");
    server = http_server_create(8080);
    if (server == NULL) {
        fprintf(stderr, "[Error] Failed to initialize server structure.\n");
        return 1;
    }
    
    http_server_add_route(server, "GET", "/", handle_home_page);
    
    printf("[App] Starting server in the background...\n");
    if (http_server_start(server, 1) != 0) {
        fprintf(stderr, "[Error] Background thread could not spawn successfully.\n");
        return 1;
    }
    
    while (keep_running) {
        static int i = 1;
        printf("%d ", i++); 
        fflush(stdout);   
        
        usleep(500000); 
    }
    
    printf("\n[App] Main counting sequence terminated. Freeing memory buffers...\n");
    http_server_free(server);
    
    printf("[App] Program exiting safely.\n");
    return 0;
}

