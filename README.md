# http_server

A small C library for running an HTTP or HTTPS server. It handles sockets, request parsing, routing, and response sending, so you can register handler functions for specific routes and let the library deal with the networking.

Includes an optional file router extension for serving static files by mapping a URL path to a file on disk.

## Features

- Basic HTTP/1.1 request parsing (method, path, version, headers, body)
- Route registration by method and path
- Runs in the foreground (blocking) or in a background thread
- Optional HTTPS support via OpenSSL
- Optional static file router built on top of the base server
- No dependencies beyond OpenSSL and pthreads

## Requirements

- A C compiler (gcc or clang)
- OpenSSL development headers and libraries (libssl, libcrypto)
- pthreads
- Linux or another POSIX-like system (uses sockets.h, unistd.h, pthread.h)

## Building

`src/http_server.c` must always be compiled and linked into your program. `src/http_server.h` is a normal header, it does not need an implementation macro.

`src/file_router.h` is a single-header module. If you use it, define `FILE_ROUTER_IMPLEMENTATION` in exactly one source file before including it.

Manual build of a program using only the base server:

```
gcc -Wall -Wextra your_app.c src/http_server.c -o your_app -lssl -lcrypto -lpthread
```

Manual build of a program that also uses the file router:

```
gcc -Wall -Wextra your_app.c src/http_server.c -o your_app -lssl -lcrypto -lpthread
```

(the file router code lives in the header itself, so no extra .c file is needed for it, just the `FILE_ROUTER_IMPLEMENTATION` define before the include)

To build all the bundled examples at once, run from the repository root:

```
bash helpers/examplebuild.sh
```

This creates an `example_compiled/` directory with four binaries: simple, filerouter, simple_https, filerouter_https.

## HTTPS certificates for local testing

The HTTPS examples expect `example/cert.pem` and `example/key.pem`. Generate a self-signed pair with:

```
bash helpers/genselfsignedssl.sh
```

This is for local testing only. Do not use a self-signed certificate in production.

## Quick start: basic server

```c
#include "src/http_server.h"
#include <stdio.h>

void handle_home_page(const http_request_t *req, int client_fd, SSL *ssl) {
    const char *body = "<html><body><h1>Hello, World!</h1></body></html>";
    http_send_response(client_fd, ssl, 200, "OK", "text/html", body);
}

int main() {
    http_server_t *server = http_server_create(8080);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    http_server_add_route(server, "GET", "/", handle_home_page);

    // background = 0, this call blocks and runs the server on the current thread
    http_server_start(server, 0);

    http_server_free(server);
    return 0;
}
```

Run it, then visit `http://localhost:8080`.

See `example/simple.c` for a full version that also runs the server in a background thread and shuts down cleanly on Ctrl+C.

## Quick start: static file server

```c
#define FILE_ROUTER_IMPLEMENTATION
#include "src/file_router.h"

int main() {
    file_router_t *router = file_router_create(8080);
    if (!router) return 1;

    file_router_add_file_route(router, "/", "example/www/index.html");

    file_router_start(router, 1); // background = 1, returns immediately

    // ... keep the process alive here ...

    file_router_free(router);
    return 0;
}
```

`file_router_add_file_route` maps one exact URL path to one file. There is no directory listing or wildcard matching, each file you want served needs its own call.

## API reference

### Core server (`http_server.h`)

```c
http_server_t *http_server_create(int port);
```
Allocates and initializes a server bound to the given port. Returns NULL on allocation failure. The socket itself is not opened until you call `http_server_start`.

```c
void http_server_enable_https(http_server_t *server, const char *cert_file, const char *key_file);
```
Turns the server into an HTTPS server using the given PEM certificate and key files. Call this before `http_server_start`. This function calls `exit()` on OpenSSL setup failure, so make sure the paths are correct before calling it.

```c
void http_server_add_route(http_server_t *server, const char *method, const char *path, route_handler_t handler);
```
Registers a handler for an exact method and path match (for example `"GET"`, `"/"`). Up to `MAX_ROUTES` (100) routes per server. Routes are matched in the order they were added, first match wins.

```c
int http_server_start(http_server_t *server, int background);
```
Starts listening and accepting connections. If `background` is non-zero, this spawns a thread and returns almost immediately. If `background` is zero, this call blocks the calling thread for as long as the server runs. Returns 0 on success, -1 if the background thread could not be created.

```c
void http_server_stop(http_server_t *server);
```
Signals the server to stop, closes the listening socket, and if it was started in the background, joins that thread before returning. Safe to call from a signal handler as shown in the examples.

```c
void http_server_free(http_server_t *server);
```
Stops the server if it is still running, frees the SSL context if one was created, and frees the server struct itself. Call this once you are done with the server.

```c
void http_send_response(int client_fd, SSL *ssl, int status_code, const char *status_text,
                        const char *content_type, const char *body);
```
Writes a full HTTP response (status line, headers, body) to the client. Pass `ssl` as NULL for plain HTTP connections, or the SSL pointer given to your handler for HTTPS connections. `body` may be NULL for an empty body. This is the only way handlers should reply to a request; there is no way to stream a response body.

### Request handler signature

```c
typedef void (*route_handler_t)(const http_request_t *req, int client_fd, SSL *ssl);
```

Each handler receives the parsed request, the raw client file descriptor, and the SSL pointer (NULL if the connection is plain HTTP). A handler is expected to call `http_send_response` exactly once before returning.

### Request struct

```c
typedef struct {
    char method[16];
    char path[1024];
    char version[16];
    http_header_t headers[MAX_HEADERS];
    int header_count;
    char *body;
    size_t body_len;
} http_request_t;
```

`headers` holds up to `MAX_HEADERS` (50) parsed header name/value pairs. `body` is heap allocated and owned by the library, it is freed automatically after your handler returns, do not free it yourself and do not keep a pointer to it past the handler call.

### File router (`file_router.h`)

```c
file_router_t *file_router_create(int port);
void file_router_free(file_router_t *router);
int file_router_add_file_route(file_router_t *router, const char *route, const char *file_path);
int file_router_start(file_router_t *router, int background);
void file_router_stop(file_router_t *router);
```

These wrap the equivalent base server functions. `file_router_add_file_route` reads the entire target file into memory on every request and serves it with a content type guessed from the file extension (html, css, js, json, png, jpg, jpeg, gif, txt, otherwise `application/octet-stream`). Up to `MAX_FILE_ROUTES` (100) mappings per router.

The file router only supports GET requests on exact, pre-registered paths, and does not resolve paths against a directory on disk, so it cannot be used to serve an arbitrary directory tree without registering every file individually.

For HTTPS with the file router, call `http_server_enable_https` on the router's `base_server` member before starting it, as shown in `example/filerouter_https.c`.

## Notes and limitations

- One request per connection: the server reads once, handles it, and closes the socket (`Connection: close`). There is no keep-alive support.
- Routing is exact string matching only. There are no path parameters, wildcards, or query string parsing.
- The request buffer is fixed at 8192 bytes. Requests larger than this will be truncated.
- This library does not sanitize file paths in any way, `file_router_add_file_route` will happily read any file the process has permission to read. Do not pass user-controlled input as the `file_path` argument.
- Not thread-safe for concurrent requests. Each accepted connection is currently handled synchronously inside the accept loop, so one slow handler will block new connections from being accepted.
- Intended for small tools, local development, and learning purposes rather than production traffic.

## License

MIT. See the LICENSE file for the full text.
