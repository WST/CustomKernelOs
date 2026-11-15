CPython 3.15.2 for MyOS ext.

Python-3.15.2.tgz
https://www.python.org/ftp/python/3.15.2/Python-3.15.2.tgz
sha256 f74e1db0eabd651cd01e4e2a00201bd41866abe74869ab716f9bf33cbbaa0943

virtualenv 20.31.2 and the wheels it imports offline:
virtualenv-20.31.2-py3-none-any.whl
sha256 36efd0d9650ee985f0cad72065001e66d49a6f24eb44d98980f630686243cf11
filelock-3.19.1-py3-none-any.whl
sha256 d38e30481def20772f5baf097c122c3babc4fcdb7e14e57049eb9d88c6dc017d
distlib-0.4.4-py2.py3-none-any.whl
sha256 cf09d55b1e85a89576b036aeb71550bfd68320f3f21761ca329eadadad918638
platformdirs-4.14.5-py3-none-any.whl
sha256 8e778c6fa421516f4d3369449f8df3b0e1b72c0e0005fe88e3ec772d0a14e750

The guest tar accepts gzip, not xz. A fresh 64 MiB disk is too small:
the recipe expects a grown volume (the default disk.img is 4 GiB,
MAX_INODES 131072). Build with make -j4. Frozen modules are produced
by _freeze_module during that make and are not stored in this tree.
