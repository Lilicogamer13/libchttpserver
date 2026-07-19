#ifndef FILE_ROUTER_H
#define FILE_ROUTER_H

#include "http_server.h"
#include <stdio.h>    
#include <stdlib.h>   
#include <string.h>   

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_FILE_ROUTES 100
#define MAX_PATH_LEN 1024

typedef struct {
    char route_path[MAX_PATH_LEN];
    char file_path[MAX_PATH_LEN];
} file_route_mapping_t;

typedef struct {
    http_server_t *base_server;
    file_route_mapping_t mappings[MAX_FILE_ROUTES];
    int mapping_count;
} file_router_t;

file_router_t *file_router_create(int port);

void file_router_free(file_router_t *router);

void file_router_enable_https(file_router_t *router, const char *cert_file, const char *key_file);

int file_router_add_file_route(file_router_t *router, const char *route, const char *file_path);

int file_router_start(file_router_t *router, int background);

void file_router_stop(file_router_t *router);

#ifdef __cplusplus
}
#endif

#ifdef FILE_ROUTER_IMPLEMENTATION

static const char *get_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    
    #if defined(_WIN32) || defined(_WIN64)
    if (stricmp(ext, ".html") == 0 || stricmp(ext, ".htm") == 0) return "text/html";
    if (stricmp(ext, ".css") == 0) return "text/css";
    if (stricmp(ext, ".js") == 0) return "application/javascript";
    if (stricmp(ext, ".json") == 0) return "application/json";
    if (stricmp(ext, ".png") == 0) return "image/png";
    if (stricmp(ext, ".jpg") == 0 || stricmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (stricmp(ext, ".gif") == 0) return "image/gif";
    if (stricmp(ext, ".txt") == 0) return "text/plain";
    #else
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    #endif
    
    return "application/octet-stream";
}

static void internal_file_serve_handler(const http_request_t *req, int client_fd, SSL *ssl) {
    extern file_router_t *global_router_ctx; 
    
    if (!global_router_ctx) {
        http_send_response(client_fd, ssl, 500, "Internal Server Error", "text/plain", "Router context uninitialized");
        return;
    }

    const char *target_file = NULL;
    for (int i = 0; i < global_router_ctx->mapping_count; i++) {
        if (strcmp(global_router_ctx->mappings[i].route_path, req->path) == 0) {
            target_file = global_router_ctx->mappings[i].file_path;
            break;
        }
    }

    if (!target_file) {
        http_send_response(client_fd, ssl, 404, "Not Found", "text/plain", "Route Mapping Lost");
        return;
    }

    FILE *f = fopen(target_file, "rb");
    if (!f) {
        http_send_response(client_fd, ssl, 404, "Not Found", "text/plain", "File not found on system disk");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *string_buffer = malloc(fsize + 1);
    if (!string_buffer) {
        fclose(f);
        http_send_response(client_fd, ssl, 500, "Internal Server Error", "text/plain", "Memory Allocation Failure");
        return;
    }

    size_t read_bytes = fread(string_buffer, 1, fsize, f);
    string_buffer[read_bytes] = '\0';
    fclose(f);

    const char *mime = get_content_type(target_file);
    http_send_response(client_fd, ssl, 200, "OK", mime, string_buffer);
    
    free(string_buffer);
}

file_router_t *global_router_ctx = NULL;

file_router_t *file_router_create(int port) {
    file_router_t *router = (file_router_t *)malloc(sizeof(file_router_t));
    if (!router) return NULL;

    router->base_server = http_server_create(port);
    if (!router->base_server) {
        free(router);
        return NULL;
    }

    router->mapping_count = 0;
    global_router_ctx = router; 
    return router;
}

void file_router_enable_https(file_router_t *router, const char *cert_file, const char *key_file) {
    if (!router) return;
    http_server_enable_https(router->base_server, cert_file, key_file);
}

void file_router_free(file_router_t *router) {
    if (!router) return;
    if (router->base_server) {
        http_server_free(router->base_server);
    }
    if (global_router_ctx == router) {
        global_router_ctx = NULL;
    }
    free(router);
}

int file_router_add_file_route(file_router_t *router, const char *route, const char *file_path) {
    if (!router || router->mapping_count >= MAX_FILE_ROUTES) return -1;

    strncpy(router->mappings[router->mapping_count].route_path, route, MAX_PATH_LEN - 1);
    strncpy(router->mappings[router->mapping_count].file_path, file_path, MAX_PATH_LEN - 1);
    router->mapping_count++;

    http_server_add_route(router->base_server, "GET", route, internal_file_serve_handler);
    return 0;
}

int file_router_start(file_router_t *router, int background) {
    if (!router) return -1;
    return http_server_start(router->base_server, background);
}

void file_router_stop(file_router_t *router) {
    if (!router) return;
    http_server_stop(router->base_server);
}

#endif
#endif
