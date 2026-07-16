#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

#define MAX_HEADERS 50
#define MAX_ROUTES 100

typedef struct {
    char name[128];
    char value[512];
} http_header_t;

typedef struct {
    char method[16];
    char path[1024];
    char version[16];
    http_header_t headers[MAX_HEADERS];
    int header_count;
    char *body;
    size_t body_len;
} http_request_t;

typedef void (*route_handler_t)(const http_request_t *req, int client_fd, SSL *ssl);

typedef struct {
    char method[16];
    char path[1024];
    route_handler_t handler;
} http_route_t;

typedef struct {
    int port;
    int server_fd;
    int is_https;
    SSL_CTX *ssl_ctx;
    http_route_t routes[MAX_ROUTES];
    int route_count;
    
    int is_running;
    unsigned long thread_id; 
} http_server_t;

http_server_t *http_server_create(int port);
void http_server_enable_https(http_server_t *server, const char *cert_file, const char *key_file);
void http_server_free(http_server_t *server);

void http_server_add_route(http_server_t *server, const char *method, const char *path, route_handler_t handler);

/* WARN: If 'background' is non-zero, this spawns a background thread and returns immediately. */
int http_server_start(http_server_t *server, int background);
void http_server_stop(http_server_t *server);

void http_send_response(int client_fd, SSL *ssl, int status_code, const char *status_text, 
                        const char *content_type, const char *body);

#ifdef __cplusplus
}
#endif

#endif
