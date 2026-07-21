#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#define BUFFER_SIZE 8192

static void init_openssl(void) {
    static int initialized = 0;
    if (!initialized) {
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        initialized = 1;
    }
}

static void cleanup_openssl(void) {
    EVP_cleanup();
}

static SSL_CTX *create_ssl_context(void) {
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

static void configure_ssl_context(SSL_CTX *ctx, const char *cert_file, const char *key_file) {
    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

static int parse_http_request(const char *raw_data, size_t data_len, http_request_t *req) {
    memset(req, 0, sizeof(http_request_t));

    const char *header_end = strstr(raw_data, "\r\n\r\n");
    if (!header_end) {
        return -1;
    }

    size_t header_len = header_end - raw_data;
    char *headers_temp = malloc(header_len + 1);
    if (!headers_temp) return -1;
    memcpy(headers_temp, raw_data, header_len);
    headers_temp[header_len] = '\0';

    char *line_saveptr = NULL;
    char *line = strtok_r(headers_temp, "\r\n", &line_saveptr);
    if (!line) {
        free(headers_temp);
        return -1;
    }

    sscanf(line, "%15s %1023s %15s", req->method, req->path, req->version);

    while ((line = strtok_r(NULL, "\r\n", &line_saveptr)) != NULL) {
        if (req->header_count >= MAX_HEADERS) break;

        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *name = line;
            char *value = colon + 1;

            while (*value == ' ') value++;
            
            strncpy(req->headers[req->header_count].name, name, sizeof(req->headers[0].name) - 1);
            strncpy(req->headers[req->header_count].value, value, sizeof(req->headers[0].value) - 1);
            req->header_count++;
        }
    }
    free(headers_temp);

    const char *body_start = header_end + 4;
    size_t parsed_header_bytes = body_start - raw_data;
    if (data_len > parsed_header_bytes) {
        req->body_len = data_len - parsed_header_bytes;
        req->body = malloc(req->body_len + 1);
        if (req->body) {
            memcpy(req->body, body_start, req->body_len);
            req->body[req->body_len] = '\0';
        }
    } else {
        req->body = NULL;
        req->body_len = 0;
    }

    return 0;
}

static void free_http_request_body(http_request_t *req) {
    if (req->body) {
        free(req->body);
        req->body = NULL;
    }
}

static int send_all(int client_fd, SSL *ssl, const char *data, size_t len) {
    if (ssl) {
        int total_sent = 0;
        while (total_sent < (int)len) {
            int sent = SSL_write(ssl, data + total_sent, len - total_sent);
            if (sent <= 0) {
                int err = SSL_get_error(ssl, sent);
                if (err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ) {
                    return -1;
                }
            } else {
                total_sent += sent;
            }
        }
        return total_sent;
    } else {
        int total_sent = 0;
        while (total_sent < (int)len) {
            int sent = send(client_fd, data + total_sent, len - total_sent, 0);
            if (sent <= 0) return -1;
            total_sent += sent;
        }
        return total_sent;
    }
}

void http_send_response(int client_fd, SSL *ssl, int status_code, const char *status_text, 
                        const char *content_type, const char *body) {
    char header_buf[2048];
    size_t body_len = body ? strlen(body) : 0;

    int header_len = snprintf(header_buf, sizeof(header_buf),
        "HTTP/1.1 %d %s\r\n"
        "Server: LibCHttpHttpsServer/2.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);

    if (header_len < 0 || (size_t)header_len >= sizeof(header_buf)) {
        return;
    }

    send_all(client_fd, ssl, header_buf, header_len);
    if (body_len > 0) {
        send_all(client_fd, ssl, body, body_len);
    }
}

static void handle_client(http_server_t *server, int client_fd) {
    SSL *ssl = NULL;

    if (server->is_https) {
        ssl = SSL_new(server->ssl_ctx);
        SSL_set_fd(ssl, client_fd);

        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            close(client_fd);
            return;
        }
    }

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = 0;

    if (ssl) {
        bytes_read = SSL_read(ssl, buffer, BUFFER_SIZE - 1);
    } else {
        bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    }

    if (bytes_read > 0) {
        http_request_t req;
        if (parse_http_request(buffer, bytes_read, &req) == 0) {
            int route_found = 0;
            for (int i = 0; i < server->route_count; i++) {
                if (strcmp(server->routes[i].method, req.method) == 0 &&
                    strcmp(server->routes[i].path, req.path) == 0) {
                    server->routes[i].handler(&req, client_fd, ssl);
                    route_found = 1;
                    break;
                }
            }

            if (!route_found) {
                http_send_response(client_fd, ssl, 404, "Not Found", "text/plain", "404 - Page Not Found");
            }
            free_http_request_body(&req);
        } else {
            http_send_response(client_fd, ssl, 400, "Bad Request", "text/plain", "400 - Bad Request");
        }
    }

    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    close(client_fd);
}

http_server_t *http_server_create(int port) {
    http_server_t *server = malloc(sizeof(http_server_t));
    if (!server) return NULL;
    server->port = port;
    server->server_fd = -1;
    server->is_https = 0;
    server->ssl_ctx = NULL;
    server->route_count = 0;
    server->is_running = 0;
    server->thread_id = 0;
    return server;
}

void http_server_enable_https(http_server_t *server, const char *cert_file, const char *key_file) {
    init_openssl();
    server->ssl_ctx = create_ssl_context();
    configure_ssl_context(server->ssl_ctx, cert_file, key_file);
    server->is_https = 1;
}

void http_server_add_route(http_server_t *server, const char *method, const char *path, route_handler_t handler) {
    if (server->route_count >= MAX_ROUTES) {
        return;
    }
    http_route_t *route = &server->routes[server->route_count++];
    strncpy(route->method, method, sizeof(route->method) - 1);
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->handler = handler;
}

static void run_server_loop(http_server_t *server) {
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server->server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("[Library Error] Socket creation failed");
        return;
    }

    if (setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("[Library Error] setsockopt failed");
        return;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server->port);

    if (bind(server->server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("[Library Error] Bind failed");
        return;
    }

    if (listen(server->server_fd, 10) < 0) {
        perror("[Library Error] Listen failed");
        return;
    }

    server->is_running = 1;
    printf("[Library] Server online on %s://localhost:%d\n", 
           server->is_https ? "https" : "http", server->port);

    while (server->is_running) {
        int client_fd = accept(server->server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_fd < 0) {
            if (!server->is_running) break;
            perror("[Library] Accept failed");
            continue;
        }
        handle_client(server, client_fd);
    }
}

static void *server_thread_func(void *arg) {
    http_server_t *server = (http_server_t *)arg;
    run_server_loop(server);
    return NULL;
}

int http_server_start(http_server_t *server, int background) {
    if (background) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, server_thread_func, server) != 0) {
            perror("[Library Error] Failed to create background thread");
            return -1;
        }
        server->thread_id = (unsigned long)thread;
        usleep(100000); 
        return 0;
    } else {
        run_server_loop(server);
        return 0;
    }
}

void http_server_stop(http_server_t *server) {
    server->is_running = 0;

    if (server->server_fd != -1) {
        shutdown(server->server_fd, SHUT_RDWR); // unblocks accept() reliably
        close(server->server_fd);
        server->server_fd = -1;
    }

    if (server->thread_id != 0) {
        pthread_join((pthread_t)server->thread_id, NULL);
        server->thread_id = 0;
    }

    printf("[Library] Server stopped safely.\n");
}

void http_server_free(http_server_t *server) {
    if (!server) return;
    if (server->is_running) {
        http_server_stop(server);
    }
    if (server->ssl_ctx) {
        SSL_CTX_free(server->ssl_ctx);
        cleanup_openssl();
    }
    free(server);
}
