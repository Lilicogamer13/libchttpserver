from setuptools import setup, Extension

_http_server = Extension(
    "_http_server",
    sources=[
        "vendor/http_server.c",
        "vendor/file_router_impl.c",
        "src/py_http_server.c",
    ],
    include_dirs=["vendor"],
    libraries=["ssl", "crypto", "pthread"],
    extra_compile_args=["-Wall"],
)

setup(
    name="pyhttpserver",
    version="0.1.0",
    description="Python bindings for the CustomCServerLibrary HTTP server",
    packages=["http_server"],
    package_dir={"http_server": "python/http_server"},
    ext_modules=[_http_server],
    python_requires=">=3.8",
)
