/*
 * _http_server - CPython C extension wrapping the vendored
 * http_server.c / http_server.h / file_router.h C library.
 *
 * Design notes (read before modifying):
 *
 *  - route_handler_t is a plain C function pointer with NO user-data
 *    parameter, so the underlying library cannot tell our trampoline
 *    which Server instance (or which Python callback) a request belongs
 *    to. We work around this with a single process-wide registry keyed
 *    by (method, path). This means: don't register the same (method,
 *    path) pair on two different Server() instances running at once.
 *
 *  - Route handlers may run on a pthread spawned by the C library
 *    (background=True) or in the calling thread (background=False).
 *    Either way that thread does not hold the GIL when it calls our
 *    trampoline, so the trampoline must acquire it (PyGILState_Ensure)
 *    before touching any Python object, and blocking C calls
 *    (start/stop/free/enable_https) release the GIL so other Python
 *    threads (and the request-handling thread trying to reacquire the
 *    GIL) are not blocked.
 *
 *  - SSL* is passed through to Python as an opaque PyCapsule. Python
 *    code cannot do anything with it except pass it back into
 *    send_response(); this mirrors what the C library itself allows.
 *
 *  - http_send_response() takes a NUL-terminated const char* body with
 *    no explicit length, so (exactly as in the C library) a body
 *    containing embedded NUL bytes will be truncated. This is a
 *    limitation of the vendored library, not something this wrapper
 *    can fix without changing its ABI.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "http_server.h"
#include "file_router.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Global route registry: dict[(method, path)] -> python callable      */
/* ------------------------------------------------------------------ */

static PyObject *g_route_registry = NULL; /* set in module init */

static PyObject *
build_headers_dict(const http_request_t *req)
{
    PyObject *headers = PyDict_New();
    if (!headers)
        return NULL;

    for (int i = 0; i < req->header_count; i++) {
        PyObject *val = PyUnicode_FromString(req->headers[i].value);
        if (!val) {
            Py_DECREF(headers);
            return NULL;
        }
        int rc = PyDict_SetItemString(headers, req->headers[i].name, val);
        Py_DECREF(val);
        if (rc < 0) {
            Py_DECREF(headers);
            return NULL;
        }
    }
    return headers;
}

static PyObject *
build_request_object(const http_request_t *req)
{
    PyObject *headers = build_headers_dict(req);
    if (!headers)
        return NULL;

    PyObject *body;
    if (req->body && req->body_len > 0) {
        body = PyBytes_FromStringAndSize(req->body, (Py_ssize_t)req->body_len);
    } else {
        body = Py_None;
        Py_INCREF(body);
    }
    if (!body) {
        Py_DECREF(headers);
        return NULL;
    }

    /* Py_BuildValue with 'N' steals references to headers/body */
    PyObject *dict = Py_BuildValue(
        "{s:s, s:s, s:s, s:N, s:N}",
        "method", req->method,
        "path", req->path,
        "version", req->version,
        "headers", headers,
        "body", body);
    return dict;
}

/* This is the single C function pointer registered for every route on
 * every Server. It looks the real Python callback up by (method, path)
 * in g_route_registry. See the file-level comment for why. */
static void
py_route_trampoline(const http_request_t *req, int client_fd, SSL *ssl)
{
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject *key = Py_BuildValue("(ss)", req->method, req->path);
    PyObject *callback = NULL;
    if (key) {
        callback = PyDict_GetItem(g_route_registry, key); /* borrowed ref */
    }

    if (!callback) {
        Py_XDECREF(key);
        PyGILState_Release(gstate);
        http_send_response(client_fd, ssl, 404, "Not Found", "text/plain",
                            "404 - No Python handler registered for this route");
        return;
    }

    PyObject *req_obj = build_request_object(req);
    PyObject *ssl_capsule = ssl ? PyCapsule_New((void *)ssl, "http_server.SSL", NULL)
                                 : (Py_INCREF(Py_None), Py_None);

    int handler_ran_ok = 0;

    if (req_obj && ssl_capsule) {
        PyObject *result = PyObject_CallFunctionObjArgs(
            callback, req_obj, PyLong_FromLong(client_fd), ssl_capsule, NULL);
        if (result) {
            Py_DECREF(result);
            handler_ran_ok = 1;
        } else {
            PyErr_Print(); /* logs to stderr, clears the error */
        }
    } else {
        PyErr_Clear();
    }

    Py_XDECREF(req_obj);
    Py_XDECREF(ssl_capsule);
    Py_XDECREF(key);
    PyGILState_Release(gstate);

    if (!handler_ran_ok) {
        /* Best-effort fallback. If the handler already sent a response
         * before raising, this will harmlessly fail to write to a
         * closed/already-used socket. */
        http_send_response(client_fd, ssl, 500, "Internal Server Error",
                            "text/plain", "500 - Python handler raised an exception");
    }
}

/* ------------------------------------------------------------------ */
/* Server type                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    http_server_t *srv;
} ServerObject;

static void
Server_dealloc(ServerObject *self)
{
    if (self->srv) {
        Py_BEGIN_ALLOW_THREADS
        http_server_free(self->srv); /* stops+joins background thread if running */
        Py_END_ALLOW_THREADS
        self->srv = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Server_init(ServerObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"port", NULL};
    int port;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i", kwlist, &port))
        return -1;

    self->srv = http_server_create(port);
    if (!self->srv) {
        PyErr_NoMemory();
        return -1;
    }
    return 0;
}

static PyObject *
Server_enable_https(ServerObject *self, PyObject *args)
{
    const char *cert_file, *key_file;
    if (!PyArg_ParseTuple(args, "ss", &cert_file, &key_file))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    http_server_enable_https(self->srv, cert_file, key_file);
    Py_END_ALLOW_THREADS

    Py_RETURN_NONE;
}

static PyObject *
Server_add_route(ServerObject *self, PyObject *args)
{
    const char *method, *path;
    PyObject *callback;
    if (!PyArg_ParseTuple(args, "ssO", &method, &path, &callback))
        return NULL;

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable");
        return NULL;
    }
    if (strlen(method) >= 16) {
        PyErr_SetString(PyExc_ValueError, "method must be < 16 chars (C library limit)");
        return NULL;
    }
    if (strlen(path) >= 1024) {
        PyErr_SetString(PyExc_ValueError, "path must be < 1024 chars (C library limit)");
        return NULL;
    }

    PyObject *key = Py_BuildValue("(ss)", method, path);
    if (!key)
        return NULL;
    if (PyDict_SetItem(g_route_registry, key, callback) < 0) {
        Py_DECREF(key);
        return NULL;
    }
    Py_DECREF(key);

    http_server_add_route(self->srv, method, path, py_route_trampoline);
    Py_RETURN_NONE;
}

static PyObject *
Server_start(ServerObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"background", NULL};
    int background = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p", kwlist, &background))
        return NULL;

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = http_server_start(self->srv, background);
    Py_END_ALLOW_THREADS

    if (rc != 0) {
        PyErr_SetString(PyExc_RuntimeError, "http_server_start() failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Server_stop(ServerObject *self, PyObject *Py_UNUSED(ignored))
{
    Py_BEGIN_ALLOW_THREADS
    http_server_stop(self->srv);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject *
Server_get_port(ServerObject *self, void *closure)
{
    (void)closure;
    return PyLong_FromLong(self->srv->port);
}

static PyObject *
Server_get_is_running(ServerObject *self, void *closure)
{
    (void)closure;
    return PyBool_FromLong(self->srv->is_running);
}

static PyGetSetDef Server_getset[] = {
    {"port", (getter)Server_get_port, NULL, "listening port", NULL},
    {"is_running", (getter)Server_get_is_running, NULL, "whether the server loop is active", NULL},
    {NULL}};

static PyMethodDef Server_methods[] = {
    {"enable_https", (PyCFunction)Server_enable_https, METH_VARARGS,
     "enable_https(cert_file, key_file)\n\nEnable TLS using a PEM cert/key pair."},
    {"add_route", (PyCFunction)Server_add_route, METH_VARARGS,
     "add_route(method, path, callback)\n\n"
     "callback(request: dict, client_fd: int, ssl: capsule|None) -> None\n"
     "The callback is responsible for calling send_response() itself."},
    {"start", (PyCFunction)Server_start, METH_VARARGS | METH_KEYWORDS,
     "start(background=True)\n\nIf background is False this blocks forever."},
    {"stop", (PyCFunction)Server_stop, METH_NOARGS,
     "stop()\n\nStop the server and join the background thread if any."},
    {NULL}};

static PyTypeObject ServerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "_http_server.Server",
    .tp_basicsize = sizeof(ServerObject),
    .tp_dealloc = (destructor)Server_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Server(port) - wraps http_server_t",
    .tp_methods = Server_methods,
    .tp_getset = Server_getset,
    .tp_init = (initproc)Server_init,
    .tp_new = PyType_GenericNew,
};

/* ------------------------------------------------------------------ */
/* FileRouter type                                                     */
/*                                                                      */
/* NOTE: file_router.h keeps a single process-wide global pointer      */
/* (global_router_ctx) internally, so only ONE FileRouter may safely   */
/* be alive at a time in the whole process, regardless of how many     */
/* Python FileRouter objects you try to create.                        */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    file_router_t *router;
} FileRouterObject;

static void
FileRouter_dealloc(FileRouterObject *self)
{
    if (self->router) {
        Py_BEGIN_ALLOW_THREADS
        file_router_free(self->router);
        Py_END_ALLOW_THREADS
        self->router = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
FileRouter_init(FileRouterObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"port", NULL};
    int port;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i", kwlist, &port))
        return -1;

    self->router = file_router_create(port);
    if (!self->router) {
        PyErr_NoMemory();
        return -1;
    }
    return 0;
}

static PyObject *
FileRouter_add_file_route(FileRouterObject *self, PyObject *args)
{
    const char *route, *file_path;
    if (!PyArg_ParseTuple(args, "ss", &route, &file_path))
        return NULL;

    int rc = file_router_add_file_route(self->router, route, file_path);
    if (rc != 0) {
        PyErr_SetString(PyExc_RuntimeError,
                         "file_router_add_file_route() failed (route table full?)");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
FileRouter_start(FileRouterObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"background", NULL};
    int background = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p", kwlist, &background))
        return NULL;

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = file_router_start(self->router, background);
    Py_END_ALLOW_THREADS

    if (rc != 0) {
        PyErr_SetString(PyExc_RuntimeError, "file_router_start() failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
FileRouter_stop(FileRouterObject *self, PyObject *Py_UNUSED(ignored))
{
    Py_BEGIN_ALLOW_THREADS
    file_router_stop(self->router);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyMethodDef FileRouter_methods[] = {
    {"add_file_route", (PyCFunction)FileRouter_add_file_route, METH_VARARGS,
     "add_file_route(route, file_path)"},
    {"start", (PyCFunction)FileRouter_start, METH_VARARGS | METH_KEYWORDS,
     "start(background=True)"},
    {"stop", (PyCFunction)FileRouter_stop, METH_NOARGS, "stop()"},
    {NULL}};

static PyTypeObject FileRouterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = "_http_server.FileRouter",
    .tp_basicsize = sizeof(FileRouterObject),
    .tp_dealloc = (destructor)FileRouter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "FileRouter(port) - wraps file_router_t. Only one instance may be "
              "active at a time (the underlying C library uses a single global).",
    .tp_methods = FileRouter_methods,
    .tp_init = (initproc)FileRouter_init,
    .tp_new = PyType_GenericNew,
};

/* ------------------------------------------------------------------ */
/* Module-level send_response()                                        */
/* ------------------------------------------------------------------ */

static PyObject *
mod_send_response(PyObject *module, PyObject *args)
{
    (void)module;
    int client_fd, status_code;
    PyObject *ssl_obj;
    const char *status_text, *content_type;
    PyObject *body_obj;

    if (!PyArg_ParseTuple(args, "iOissO", &client_fd, &ssl_obj, &status_code,
                           &status_text, &content_type, &body_obj))
        return NULL;

    SSL *ssl = NULL;
    if (ssl_obj != Py_None) {
        if (!PyCapsule_IsValid(ssl_obj, "http_server.SSL")) {
            PyErr_SetString(PyExc_TypeError,
                             "ssl must be None or the SSL capsule passed into your route handler");
            return NULL;
        }
        ssl = (SSL *)PyCapsule_GetPointer(ssl_obj, "http_server.SSL");
    }

    PyObject *body_bytes_owned = NULL;
    const char *body_cstr = NULL;
    if (body_obj == Py_None) {
        body_cstr = NULL;
    } else if (PyUnicode_Check(body_obj)) {
        body_bytes_owned = PyUnicode_AsUTF8String(body_obj);
        if (!body_bytes_owned)
            return NULL;
        body_cstr = PyBytes_AsString(body_bytes_owned);
    } else if (PyBytes_Check(body_obj)) {
        body_cstr = PyBytes_AsString(body_obj);
    } else {
        PyErr_SetString(PyExc_TypeError, "body must be str, bytes, or None");
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    http_send_response(client_fd, ssl, status_code, status_text, content_type, body_cstr);
    Py_END_ALLOW_THREADS

    Py_XDECREF(body_bytes_owned);
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"send_response", mod_send_response, METH_VARARGS,
     "send_response(client_fd, ssl, status_code, status_text, content_type, body)\n\n"
     "ssl must be the capsule (or None) your route handler received. body may be\n"
     "str, bytes, or None. NOTE: bytes bodies containing embedded NUL bytes are\n"
     "truncated at the NUL - this is a limitation of the underlying C library,\n"
     "which sends the body via strlen()."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_http_server",
    "Low-level CPython binding for the vendored http_server.c/file_router.h C library.",
    -1,
    module_methods,
};

PyMODINIT_FUNC
PyInit__http_server(void)
{
    g_route_registry = PyDict_New();
    if (!g_route_registry)
        return NULL;

    if (PyType_Ready(&ServerType) < 0)
        return NULL;
    if (PyType_Ready(&FileRouterType) < 0)
        return NULL;

    PyObject *m = PyModule_Create(&moduledef);
    if (!m)
        return NULL;

    Py_INCREF(&ServerType);
    if (PyModule_AddObject(m, "Server", (PyObject *)&ServerType) < 0) {
        Py_DECREF(&ServerType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&FileRouterType);
    if (PyModule_AddObject(m, "FileRouter", (PyObject *)&FileRouterType) < 0) {
        Py_DECREF(&FileRouterType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
