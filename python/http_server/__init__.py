"""
http_server - Pythonic wrapper around the compiled `_http_server` C extension,
which in turn wraps the vendored C library (src/http_server.c / file_router.h).

Quick start
-----------
    import http_server as hs

    app = hs.Server(8080)

    @app.route("GET", "/")
    def index(req, respond):
        respond(200, "OK", "text/plain", "Hello from Python!")

    app.start(background=False)   # blocks; Ctrl+C to stop

Lower-level access (matches the C API 1:1) is available via the compiled
`_http_server` module directly: `_http_server.Server`, `_http_server.FileRouter`,
`_http_server.send_response`.

Known limitations (inherited from the underlying C library, not this wrapper):
  * Route handler bodies are sent via strlen(), so bytes/str bodies containing
    an embedded NUL byte get truncated there.
  * The C library has no per-server context in its route-handler callback, so
    route matching is global-process-wide by (method, path). Don't register
    the same (method, path) pair on two Server() instances running at once.
  * FileRouter relies on a single process-wide global in the C library, so
    only one FileRouter may be alive at a time in the whole process.
"""

from __future__ import annotations

import functools
from typing import Callable, Optional

import _http_server

__all__ = ["Server", "FileRouter", "send_response", "Request"]

send_response = _http_server.send_response


class Request(dict):
    """A dict subclass, so req["path"] and req.path both work.

    Keys: method, path, version, headers (dict), body (bytes | None)
    """

    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError as exc:
            raise AttributeError(name) from exc


class Server:
    """Pythonic wrapper around `_http_server.Server`.

    Two ways to write handlers:

    1. Flask-style, using `respond()` (recommended - can't forget arguments):

        @app.route("GET", "/hello")
        def hello(req, respond):
            respond(200, "OK", "text/plain", "hi")

    2. Raw, matching the C callback signature exactly:

        def hello(req, client_fd, ssl):
            http_server.send_response(client_fd, ssl, 200, "OK", "text/plain", "hi")

        app.add_route("GET", "/hello", hello)   # bypasses the respond() wrapping
    """

    def __init__(self, port: int):
        self._impl = _http_server.Server(port)

    # -- raw route registration, 1:1 with the C API -----------------
    def add_route(self, method: str, path: str, handler: Callable) -> None:
        """handler(request: dict, client_fd: int, ssl) -> None"""
        self._impl.add_route(method, path, handler)

    # -- convenience decorator with a respond() callback -------------
    def route(self, method: str, path: str):
        def decorator(fn: Callable):
            @functools.wraps(fn)
            def wrapped(req_dict, client_fd, ssl):
                request = Request(req_dict)

                def respond(status_code: int, status_text: str,
                            content_type: str, body=""):
                    send_response(client_fd, ssl, status_code, status_text,
                                  content_type, body)

                fn(request, respond)

            self._impl.add_route(method, path, wrapped)
            return fn

        return decorator

    def enable_https(self, cert_file: str, key_file: str) -> None:
        self._impl.enable_https(cert_file, key_file)

    def start(self, background: bool = True) -> None:
        self._impl.start(background=background)

    def stop(self) -> None:
        self._impl.stop()

    @property
    def port(self) -> int:
        return self._impl.port

    @property
    def is_running(self) -> bool:
        return self._impl.is_running

    # -- context manager: always starts in the background ------------
    def __enter__(self):
        self.start(background=True)
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop()
        return False


class FileRouter:
    """Pythonic wrapper around `_http_server.FileRouter`.

    NOTE: the underlying C library keeps a single process-wide global for
    the "current" file router, so only one FileRouter should be alive at
    a time in the whole process.
    """

    def __init__(self, port: int):
        self._impl = _http_server.FileRouter(port)

    def enable_https(self, cert_file: str, key_file: str) -> None:
        """Enable TLS using a PEM cert/key pair. Call before start()."""
        self._impl.enable_https(cert_file, key_file)

    def add_file_route(self, route: str, file_path: str) -> None:
        self._impl.add_file_route(route, file_path)

    def start(self, background: bool = True) -> None:
        self._impl.start(background=background)

    def stop(self) -> None:
        self._impl.stop()

    def __enter__(self):
        self.start(background=True)
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop()
        return False
