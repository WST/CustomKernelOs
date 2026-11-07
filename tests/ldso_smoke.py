#!/usr/bin/env python3
"""QEMU: /lib/ld.so загружает программу с разделяемыми библиотеками. Диск — копия."""
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


def main():
    image = BUILD / "ldso-smoke.img"
    shutil.copyfile(ROOT / "disk.img", image)
    myfs = [str(BUILD / "myfs"), str(image)]
    subprocess.run(myfs + ["rm", "/etc/accounts"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for directory in ("/lib", "/usr", "/usr/lib", "/tmp"):
        subprocess.run(myfs + ["mkdir", directory], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(myfs + ["put", str(BUILD / "ld.so"), "/lib/ld.so"], check=True)
    subprocess.run(myfs + ["put", str(BUILD / "libadd.so"), "/lib/libadd.so"], check=True)
    subprocess.run(myfs + ["put", str(BUILD / "libb.so"), "/lib/libb.so"], check=True)
    subprocess.run(myfs + ["put", str(BUILD / "dynuse"), "/tmp/dynuse"], check=True)

    log = BUILD / "ldso-smoke.serial.log"
    log.write_text("")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-m", "256M", "-display", "none",
         "-drive", "format=raw,file=" + str(BUILD / "os.img") + ",index=0",
         "-drive", "format=raw,file=" + str(image) + ",index=1",
         "-serial", "file:" + str(log), "-monitor", "stdio", "-no-reboot"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        login(proc, log)
        previous = log.stat().st_size
        send_line(proc, "/tmp/dynuse")
        deadline = time.monotonic() + 40
        while True:
            time.sleep(0.1)
            text = clean(log.read_text(errors="replace"))
            if log.stat().st_size > previous and re.search(r"(?:^|\n)root:[^\n]*\$ *$", text):
                break
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("timeout after dynuse:\n" + text[-4000:])
        if "add ok" not in text or "PANIC" in text or "ld.so:" in text:
            raise RuntimeError(text[-4000:])
        print("ldso: OK")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
