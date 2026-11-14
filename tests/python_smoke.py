"""Собрать CPython рецептом ext внутри QEMU и проверить python3, pip, venv, virtualenv.

Архив и колёса уже проверены python_build.py. Строки fetch в рецепте закомментированы.
"""
import pathlib, subprocess, tempfile, time, re, sys, shutil
ROOT = pathlib.Path(__file__).resolve().parents[1]
B = ROOT / "build"
ARCHIVE = B / "python-port/Python-3.15.2.tgz"
WHEELS = [
    "virtualenv-20.31.2-py3-none-any.whl",
    "filelock-3.19.1-py3-none-any.whl",
    "distlib-0.4.4-py2.py3-none-any.whl",
    "platformdirs-4.14.5-py3-none-any.whl",
]

def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

def main():
    if not ARCHIVE.exists():
        raise SystemExit("missing " + str(ARCHIVE) + " (run make python-build first)")
    wheel_dir = B / "python-native"
    # Колёса качает рецепт; для офлайн-прогона они должны лежать рядом с архивом.
    missing = [name for name in WHEELS if not (wheel_dir / name).exists() and not (B / "python-port" / name).exists()]
    port = B / "python-port"
    port.mkdir(parents=True, exist_ok=True)
    if missing:
        import urllib.request
        urls = {
            "virtualenv-20.31.2-py3-none-any.whl": "https://files.pythonhosted.org/packages/f3/40/b1c265d4b2b62b58576588510fc4d1fe60a86319c8de99fd8e9fec617d2c/virtualenv-20.31.2-py3-none-any.whl",
            "filelock-3.19.1-py3-none-any.whl": "https://files.pythonhosted.org/packages/42/14/42b2651a2f46b022ccd948bca9f2d5af0fd8929c4eec235b8d6d844fbe67/filelock-3.19.1-py3-none-any.whl",
            "distlib-0.4.4-py2.py3-none-any.whl": "https://files.pythonhosted.org/packages/c1/2b/238f02a4927fa2e27aa995371d240a468b52e42eae44a678434aff736b38/distlib-0.4.4-py2.py3-none-any.whl",
            "platformdirs-4.14.5-py3-none-any.whl": "https://files.pythonhosted.org/packages/7c/ae/ecbbbfd0e4343a6a521d49b3c8656fb4643dee2a4f6eaf941cf3280391a8/platformdirs-4.14.5-py3-none-any.whl",
        }
        for name in missing:
            dest = port / name
            if not dest.exists():
                urllib.request.urlretrieve(urls[name], dest)
    with tempfile.TemporaryDirectory(prefix="myos-python-") as folder:
        folder = pathlib.Path(folder)
        disk = folder / "data.img"
        log = folder / "serial"
        log.write_text("")
        with disk.open("wb") as f:
            f.truncate(1024 * 1048576)
        run("make", "-j4", "build/os.img", "install", "DISK=" + str(disk),
            "EXT_AUTOINSTALL_PACKAGES=", "SSH_AUTHORIZED_KEY=", "SSH_AUTHORIZED_KEY_FILE=",
            stdout=subprocess.DEVNULL)
        def fs(*args):
            return run(B / "myfs", disk, *args, stdout=subprocess.DEVNULL)
        fs("mkdir", "/tmp/ext/python")
        fs("put", ARCHIVE, "/tmp/ext/python/Python-3.15.2.tgz")
        for name in WHEELS:
            src = port / name if (port / name).exists() else wheel_dir / name
            fs("put", src, "/tmp/ext/python/" + name)
        recipe = folder / "setup.inst"
        lines = []
        for line in (ROOT / "ext/instructions/python/setup.inst").read_text().splitlines():
            lines.append("# preloaded " + line if line.startswith("fetch ") else line)
        recipe.write_text("\n".join(lines) + "\n")
        fs("put", recipe, "/ext/instructions/python/setup.inst")
        check = folder / "check.py"
        check.write_text("print(1+1)\n")
        fs("put", check, "/tmp/check.py")
        q = subprocess.Popen(
            ["qemu-system-i386", "-m", "1024M", "-display", "none", "-no-reboot", "-nic", "none",
             "-drive", "cache=unsafe,format=raw,file=" + str(B / "os.img") + ",index=0",
             "-drive", "cache=unsafe,format=raw,file=" + str(disk) + ",index=1",
             "-serial", "file:" + str(log), "-monitor", "stdio"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        def text():
            return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", log.read_text(errors="replace")).replace("\r", "")
        def wait(marker, after=0, timeout=180):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if marker in text()[after:]:
                    return
                if q.poll() is not None:
                    raise AssertionError(q.stderr.read() + text()[-3000:])
                time.sleep(0.1)
            raise AssertionError(marker + " missing:\n" + text()[-4000:])
        def key(code):
            q.stdin.write("sendkey " + code + " 10\n")
            q.stdin.flush()
            time.sleep(0.06)
        def send(s, marker, timeout=180):
            start = len(text())
            for c in s + "\n":
                key({"\n": "ret", " ": "spc", "/": "slash", ".": "dot", "-": "minus",
                     "'": "apostrophe", "(": "shift-9", ")": "shift-0", "+": "shift-equal"}.get(c, c))
            if marker:
                wait(marker, start, timeout)
        try:
            wait("New password:")
            send("pythontest1", "Confirm password:")
            send("pythontest1", "Login [root]:")
            send("", "Password:")
            send("pythontest1", "root:/")
            send("ext install python", "installed python", 7200)
            send("python3 /tmp/check.py", "2", 120)
            send("python3 -m pip --version", "pip", 120)
            send("python3 -m venv /tmp/v", "root:/", 180)
            send("virtualenv /tmp/e", "root:/", 180)
            send("ls /tmp/v/bin", "python", 60)
            send("ls /tmp/e/bin", "python", 60)
        finally:
            q.terminate()
            try:
                q.wait(timeout=5)
            except subprocess.TimeoutExpired:
                q.kill()
                q.wait()
            (B / "python-native.log").write_text(text())
    print("PASS: ext builds CPython 3.15.2; arithmetic, pip, venv and virtualenv")

if __name__ == "__main__":
    main()
