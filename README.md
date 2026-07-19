# pyhttpserver

Python bindings for the vendored `http_server.c` / `file_router.h` C library
(original: `CustomCServerLibrary/2.0`, see `vendor/`), built as a real CPython
C extension (`_http_server`) with a Pythonic wrapper on top (`http_server`).

## Build & install

Requires: gcc/clang, Python 3.8+ dev headers, OpenSSL dev headers.

```bash
# Debian/Ubuntu, if you don't already have these:
sudo apt-get install -y build-essential python3-dev libssl-dev

# from this directory:
pip install .
```

For iterative development (rebuild in place, no install):

```bash
python3 setup.py build_ext --inplace
```

This produces `_http_server*.so` in the current directory; you can then
`import http_server` directly from here without installing.

## Quick start

```python
import http_server as hs

app = hs.Server(8080)

@app.route("GET", "/")
def index(req, respond):
    respond(200, "OK", "text/plain", "Hello from Python!")

@app.route("POST", "/echo")
def echo(req, respond):
    respond(200, "OK", "text/plain", req.body.decode() if req.body else "")

app.start(background=False)   # blocks; Ctrl+C to stop
```

`req` is a dict-like `Request` object with `method`, `path`, `version`,
`headers` (dict), `body` (`bytes | None`). `respond(status_code, status_text,
content_type, body)` sends the response; `body` may be `str`, `bytes`, or
`None`.

There's also a context-manager form (always starts in the background):

```python
with hs.Server(8080) as app:
    ...
    time.sleep(60)
```

Serving static files (`FileRouter`, wraps `file_router.h`):

```python
router = hs.FileRouter(8080)
router.add_file_route("/", "./www/index.html")
router.start(background=False)
```

HTTPS:

```python
app.enable_https("cert.pem", "key.pem")
```

Lower-level 1:1 API (matches the C functions almost exactly) is available via
`_http_server.Server`, `_http_server.FileRouter`, `_http_server.send_response`
if you want to bypass the `respond()` convenience wrapper.

## Two bugs fixed in the vendored C during porting

These weren't introduced by the binding — they're pre-existing bugs in the
supplied `http_server.c` that would have made a faithful, unmodified port
unreliable from Python (and from C, for that matter). Both are marked
`PATCH (python binding)` in `vendor/http_server.c` with an explanation
in-line.

1. **`http_server_stop()` could hang forever.** The background-thread accept
   loop used a plain blocking `accept()`. Closing the listening socket from
   another thread (which is what `http_server_stop()` does) doesn't reliably
   wake a thread blocked in `accept()` on Linux, so the `pthread_join()` in
   `http_server_stop()`/`http_server_free()` would deadlock. Fixed by
   `select()`-polling the socket with a 200ms timeout before `accept()`, so
   the loop periodically re-checks `is_running`.

2. **Request bodies could be silently truncated/dropped.** `handle_client()`
   did exactly one `recv()`/`SSL_read()` call and assumed the whole request
   (headers *and* body) always arrived in that single read. TCP doesn't
   guarantee that — under real-world timing a POST body can legitimately
   arrive in a second packet. This was reproducible (~1 in 5 requests) with
   a plain `urllib` POST in testing. Fixed by looping reads until the header
   terminator is seen, then continuing to read (capped at 10MB) until the
   full `Content-Length` has actually been received.

## Known limitations (inherited from the C library's design, not fixed)

- **No per-server context in route callbacks.** `route_handler_t` is a bare
  C function pointer with no user-data slot, so the Python binding can't
  tell which `Server` instance a request belongs to — it dispatches by a
  single process-wide `(method, path)` registry. Don't register the same
  `(method, path)` pair on two `Server` instances running at the same time.
- **`FileRouter` is a process-wide singleton.** `file_router.h` stores its
  state in one global (`global_router_ctx`), so only one `FileRouter` may be
  alive at a time in the whole process, even across unrelated `Server`
  instances.
- **Response bodies are sent via `strlen()`.** `http_send_response()` takes
  `const char *body` with no explicit length, so a `bytes` body containing
  an embedded NUL byte will be truncated at that NUL.
- Fixed-size buffers from the original C API carry over: methods must be
  under 16 chars, paths under 1024 chars, headers under 50 per request, and
  at most 100 routes per server (raises `ValueError`/silently no-ops at the
  C layer respectively, per the original code).
