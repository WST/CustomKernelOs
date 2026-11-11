#!/usr/bin/env python3
"""QEMU: динамические /bin, libc.so от gcc и чужая .so. Диск — копия."""
import pathlib
import re
import shutil
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def clean(text):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text).replace("\r", "")


def wait_for(proc, log, marker, timeout=40, start=0):
    deadline = time.monotonic() + timeout
    while True:
        text = clean(log.read_text(errors="replace"))
        if marker in text[start:]:
            return text
        if proc.poll() is not None:
            raise RuntimeError(proc.stderr.read() or text[-2000:])
        if time.monotonic() > deadline:
            raise RuntimeError("timeout waiting for %r:\n%s" % (marker, text[-4000:]))
        time.sleep(0.1)


def send_line(proc, text):
    names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
    for ch in text:
        proc.stdin.write("sendkey " + names.get(ch, ch) + " 10\n")
        proc.stdin.flush()
        time.sleep(0.03)
    proc.stdin.write("sendkey ret 10\n")
    proc.stdin.flush()


def login(proc, log):
    wait_for(proc, log, "New password:")
    send_line(proc, "secret123")
    wait_for(proc, log, "Confirm password:")
    send_line(proc, "secret123")
    pos = log.stat().st_size
    wait_for(proc, log, "Login", start=pos)
    send_line(proc, "")
    pos = log.stat().st_size
    wait_for(proc, log, "Password:", start=pos)
    send_line(proc, "secret123")
    wait_for(proc, log, "root:/")


def run(proc, log, command):
    previous = log.stat().st_size
    send_line(proc, command)
    deadline = time.monotonic() + 40
    while True:
        time.sleep(0.1)
        text = clean(log.read_text(errors="replace"))
        if log.stat().st_size > previous and re.search(r"(?:^|\n)root:[^\n]*\$ *$", text):
            return text
        if proc.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("timeout after " + command + ":\n" + text[-4000:])


def main():
    image = BUILD / "dynsys-smoke.img"
    shutil.copyfile(ROOT / "disk.img", image)
    myfs = [str(BUILD / "myfs"), str(image)]
    subprocess.run(myfs + ["rm", "/etc/accounts"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for directory in ("/lib", "/usr", "/usr/lib", "/tmp"):
        subprocess.run(myfs + ["mkdir", directory], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    puts = [
        (BUILD / "ld.so", "/lib/ld.so"),
        (BUILD / "user/libc.so", "/lib/libc.so"),
        (BUILD / "user/libgfx.so", "/lib/libgfx.so"),
        (BUILD / "libgccadd.so", "/lib/libgccadd.so"),
        (BUILD / "libadd.so", "/lib/libadd.so"),
        (BUILD / "user/echo", "/bin/echo"),
        (BUILD / "user/sh", "/bin/sh"),
        (BUILD / "gfxuse", "/tmp/gfxuse"),
        (BUILD / "cc-gccso", "/tmp/cc-gccso"),
        (BUILD / "gcc-ccso", "/tmp/gcc-ccso"),
    ]
    for src, dst in puts:
        subprocess.run(myfs + ["put", str(src), dst], check=True)

    log = BUILD / "dynsys-smoke.serial.log"
    log.write_text("")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-m", "256M", "-display", "none",
         "-drive", "format=raw,file=" + str(BUILD / "os.img") + ",index=0",
         "-drive", "format=raw,file=" + str(image) + ",index=1",
         "-serial", "file:" + str(log), "-monitor", "stdio", "-no-reboot"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        login(proc, log)
        text = run(proc, log, "echo dyn-ok")
        text = run(proc, log, "/tmp/gfxuse")
        text = run(proc, log, "/tmp/cc-gccso")
        text = run(proc, log, "/tmp/gcc-ccso")
        missing = [m for m in ("dyn-ok", "gfx ok", "gccso ok", "ccso ok") if m not in text]
        if missing or "PANIC" in text or "ld.so:" in text:
            raise RuntimeError("missing %s\n%s" % (missing, text[-4000:]))
        print("dynsys: OK")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
