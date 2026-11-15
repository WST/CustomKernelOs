"""Собрать рецепт CPython тем же cc-host и libc, что использует гость.

Запуск из корня репозитория: python3 tests/python_build.py
Архив сверяется с ext/instructions/python/README.txt.
_freeze_module для заголовков запускается как -linux-сборка: гостевой
бинарник на хосте не исполняется. Итоговый python3 — формат MyOS.
"""
from pathlib import Path
import concurrent.futures, hashlib, shutil, subprocess, re, os
ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "ext/instructions/python"
BUILD = ROOT / "build/python-native"
SRC_TGZ = Path("/tmp/cpython-spike/Python-3.15.2.tgz")
SHA256 = "f74e1db0eabd651cd01e4e2a00201bd41866abe74869ab716f9bf33cbbaa0943"
DROP = {"Modules/_ssl.o", "Modules/_hashopenssl.o", "Modules/zlibmodule.o",
        "Modules/config.o", "Programs/python.o", "Modules/getpath.o", "Python/frozen.o"}

def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)

def main():
    archive = ROOT / "build/python-port/Python-3.15.2.tgz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if SRC_TGZ.exists() and not archive.exists():
        shutil.copyfile(SRC_TGZ, archive)
    if not archive.exists():
        import urllib.request
        urllib.request.urlretrieve(
            "https://www.python.org/ftp/python/3.15.2/Python-3.15.2.tgz", archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
        raise SystemExit(f"Python source checksum mismatch: {digest}")
    src = BUILD / "Python-3.15.2"
    if not src.exists():
        import tarfile
        with tarfile.open(archive) as tf:
            tf.extractall(BUILD, filter="data")
    for name, dest in (
        ("pyconfig.h", "pyconfig.h"),
        ("Makefile", "Makefile"),
        ("config.c", "Modules/config.c"),
        ("pyinstall.c", "pyinstall.c"),
        ("pyexpat_stub.c", "Modules/pyexpat.c"),
    ):
        data = (PACKAGE / name).read_bytes()
        target = src / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    sysroot = BUILD / "sysroot"
    (sysroot / "lib").mkdir(parents=True, exist_ok=True)
    if (sysroot / "include").exists():
        shutil.rmtree(sysroot / "include")
    shutil.copytree(ROOT / "build/ccroot/include", sysroot / "include")
    for name, srcpath in (
        ("libc.a", ROOT / "build/mlib/libc.a"),
        ("libm.so", ROOT / "build/user/libm.so"),
        ("libpthread.so", ROOT / "build/user/libpthread.so"),
        ("libz.so", ROOT / "build/user/libz.so"),
        ("libssl.so", ROOT / "build/user/libssl.so"),
        ("libcrypto.so", ROOT / "build/user/libcrypto.so"),
    ):
        shutil.copyfile(srcpath, sysroot / "lib" / name)
    cc = [ROOT / "build/cc-host", "-B", sysroot, "-I.", "-IInclude", "-IInclude/internal"]
    linux = [ROOT / "build/cc-host", "-B", ROOT / "build/ccroot", "-linux",
             "-I.", "-IInclude", "-IInclude/internal"]
    make = (PACKAGE / "Makefile").read_text()
    objects = list(dict.fromkeys(re.findall(r"[A-Za-z0-9_./-]+\.o\b", make)))
    objects = [o for o in objects if o not in ("Python/frozen.o", "Modules/getpath.o")]
    stamp = max(p.stat().st_mtime for p in PACKAGE.iterdir() if p.is_file())
    stamp = max(stamp, (ROOT / "build/cc-host").stat().st_mtime)
    logs = BUILD / "logs"
    logs.mkdir(exist_ok=True)

    def compile_one(obj):
        c = obj[:-2] + ".c"
        dest = src / obj
        if dest.exists() and dest.stat().st_mtime >= max(stamp, (src / c).stat().st_mtime):
            return None
        dest.parent.mkdir(parents=True, exist_ok=True)
        log = logs / (obj.replace("/", "_") + ".log")
        with log.open("w") as out:
            p = subprocess.run([str(a) for a in cc + ["-c", c, "-o", obj]],
                               cwd=src, stdout=out, stderr=subprocess.STDOUT)
        if p.returncode:
            return obj + "\n" + log.read_text()[-2000:]
        return None

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        errors = [e for e in pool.map(compile_one, objects) if e]
    if errors:
        print("\n".join(errors[:8]))
        raise SystemExit(f"{len(errors)} of {len(objects)} translation units failed")
    print(f"Compiled {len(objects)} CPython units", flush=True)

    linux_dir = BUILD / "linux"
    freeze_objs = [o for o in objects if o not in DROP and not o.startswith("pyinstall")]
    freeze_cfg = BUILD / "config_freeze.c"
    freeze_cfg.write_text(config_without_ssl())
    def compile_linux(obj):
        c = str(freeze_cfg) if obj == "config_freeze.o" else obj[:-2] + ".c"
        dest = linux_dir / obj
        source = Path(c) if obj == "config_freeze.o" else src / c
        if dest.exists() and dest.stat().st_mtime >= max(stamp, source.stat().st_mtime):
            return None
        dest.parent.mkdir(parents=True, exist_ok=True)
        log = logs / ("linux_" + obj.replace("/", "_") + ".log")
        cmd = linux + ["-c", c, "-o", str(dest)]
        with log.open("w") as out:
            p = subprocess.run([str(a) for a in cmd], cwd=src, stdout=out, stderr=subprocess.STDOUT)
        if p.returncode:
            return obj + "\n" + log.read_text()[-2000:]
        return None
    units = freeze_objs + ["config_freeze.o", "Programs/_freeze_module.o", "Modules/getpath_noop.o"]
    # _freeze_module and getpath_noop are already in objects; don't double-compile the check
    units = list(dict.fromkeys(units))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        errors = [e for e in pool.map(compile_linux, units) if e]
    if errors:
        print("\n".join(errors[:8]))
        raise SystemExit(f"{len(errors)} linux units failed")
    link_objs = [linux_dir / o for o in units]
    freeze_bin = BUILD / "_freeze_module"
    run(linux + ["-o", freeze_bin] + link_objs + ["-lpthread"], cwd=src)
    frozen = [
        ("importlib._bootstrap", "Lib/importlib/_bootstrap.py", "Python/frozen_modules/importlib._bootstrap.h"),
        ("importlib._bootstrap_external", "Lib/importlib/_bootstrap_external.py", "Python/frozen_modules/importlib._bootstrap_external.h"),
        ("zipimport", "Lib/zipimport.py", "Python/frozen_modules/zipimport.h"),
        ("abc", "Lib/abc.py", "Python/frozen_modules/abc.h"),
        ("codecs", "Lib/codecs.py", "Python/frozen_modules/codecs.h"),
        ("io", "Lib/io.py", "Python/frozen_modules/io.h"),
        ("_collections_abc", "Lib/_collections_abc.py", "Python/frozen_modules/_collections_abc.h"),
        ("_sitebuiltins", "Lib/_sitebuiltins.py", "Python/frozen_modules/_sitebuiltins.h"),
        ("genericpath", "Lib/genericpath.py", "Python/frozen_modules/genericpath.h"),
        ("ntpath", "Lib/ntpath.py", "Python/frozen_modules/ntpath.h"),
        ("posixpath", "Lib/posixpath.py", "Python/frozen_modules/posixpath.h"),
        ("os", "Lib/os.py", "Python/frozen_modules/os.h"),
        ("site", "Lib/site.py", "Python/frozen_modules/site.h"),
        ("stat", "Lib/stat.py", "Python/frozen_modules/stat.h"),
        ("linecache", "Lib/linecache.py", "Python/frozen_modules/linecache.h"),
        ("importlib.util", "Lib/importlib/util.py", "Python/frozen_modules/importlib.util.h"),
        ("importlib.machinery", "Lib/importlib/machinery.py", "Python/frozen_modules/importlib.machinery.h"),
        ("runpy", "Lib/runpy.py", "Python/frozen_modules/runpy.h"),
        ("__hello__", "Lib/__hello__.py", "Python/frozen_modules/__hello__.h"),
        ("__phello__", "Lib/__phello__/__init__.py", "Python/frozen_modules/__phello__.h"),
        ("__phello__.ham", "Lib/__phello__/ham/__init__.py", "Python/frozen_modules/__phello__.ham.h"),
        ("__phello__.ham.eggs", "Lib/__phello__/ham/eggs.py", "Python/frozen_modules/__phello__.ham.eggs.h"),
        ("__phello__.spam", "Lib/__phello__/spam.py", "Python/frozen_modules/__phello__.spam.h"),
        ("frozen_only", "Tools/freeze/flag.py", "Python/frozen_modules/frozen_only.h"),
        ("getpath", "Modules/getpath.py", "Python/frozen_modules/getpath.h"),
    ]
    for name, inp, out in frozen:
        run([freeze_bin, name, inp, out], cwd=src)
    print("Froze", len(frozen), "modules", flush=True)
    run(cc + ["-c", "Python/frozen.c", "-o", "Python/frozen.o"], cwd=src)
    run(cc + ["-c", "Modules/getpath.c", "-o", "Modules/getpath.o",
              "-DPREFIX=\"/ext\"", "-DEXEC_PREFIX=\"/ext\"", "-DVERSION=\"3.15\"",
              "-DVPATH=\".\"", "-DPLATLIBDIR=\"lib\""], cwd=src)
    run(cc + ["-c", "Programs/python.c", "-o", "Programs/python.o"], cwd=src)
    for line in make.splitlines():
        if line.startswith("\tar rc "):
            run(line.strip().split(), cwd=src, stdout=subprocess.DEVNULL)
    bins = BUILD / "bin"
    bins.mkdir(exist_ok=True)
    final = None
    for line in make.splitlines():
        if line.startswith("\t$(CC) -o /ext/bin/python3 "):
            final = line.strip().replace("$(LIBS)", "-lpthread -lssl -lcrypto -lz").split()[1:]
            final[1] = str(bins / "python3")
    if not final:
        raise SystemExit("link line missing")
    run(cc + final, cwd=src)
    run(cc + ["-o", bins / "pyinstall", "pyinstall.c"], cwd=src)
    print("Python native MyOS link: PASS")

def config_without_ssl():
    text = (PACKAGE / "config.c").read_text()
    drop = ("PyInit__ssl", "PyInit__hashlib", "PyInit_zlib", '"_ssl"', '"_hashlib"', '"zlib"')
    return "\n".join(line for line in text.splitlines()
                     if not any(tok in line for tok in drop)) + "\n"

if __name__ == "__main__":
    main()
