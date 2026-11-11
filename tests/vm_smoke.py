#!/usr/bin/env python3
"""QEMU: copy-on-write, mmap и запуск ET_DYN через PT_INTERP. Диск — копия."""
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
    """На копии диска нет учёток: задаём пароль и входим."""
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


def run(image, commands, expected):
    log = BUILD / "vm-smoke.serial.log"
    log.write_text("")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-m", "256M", "-display", "none",
         "-drive", "format=raw,file=" + str(BUILD / "os.img") + ",index=0",
         "-drive", "format=raw,file=" + str(image) + ",index=1",
         "-serial", "file:" + str(log), "-monitor", "stdio", "-no-reboot"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        login(proc, log)
        for command in commands:
            previous = log.stat().st_size
            send_line(proc, command)
            deadline = time.monotonic() + 40
            while True:
                time.sleep(0.1)
                text = clean(log.read_text(errors="replace"))
                if log.stat().st_size > previous and re.search(r"(?:^|\n)root:[^\n]*\$ *$", text):
                    break
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("timeout after " + command + ":\n" + text[-4000:])
        text = clean(log.read_text(errors="replace"))
        missing = [m for m in expected if m not in text]
        if missing or "PANIC" in text:
            raise RuntimeError("missing %s\n%s" % (missing, text[-4000:]))
        print("vm-smoke: OK")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    subprocess.run(["make", "-C", str(ROOT), "build/user/libmyos.a", "build/os.img", "build/myfs"], check=True)
    cc = ["gcc", "-m32", "-ffreestanding", "-fno-pie", "-fno-pic", "-fno-stack-protector",
          "-fno-builtin", "-nostdlib", "-nostdinc", "-mno-sse", "-mno-mmx", "-O2", "-Wall",
          "-std=gnu11", "-I" + str(ROOT / "user/include"), "-I" + str(ROOT / "user/lib"),
          "-c", str(ROOT / "tests/vmtest.c"), "-o", str(BUILD / "vmtest.o")]
    subprocess.run(cc, check=True)
    subprocess.run(["ld", "-m", "elf_i386", "-T", str(ROOT / "user/user.ld"), "-nostdlib", "-s",
                    "--gc-sections", str(BUILD / "vmtest.o"), str(BUILD / "user/libmyos.a"),
                    "-o", str(BUILD / "vmtest")], check=True)
    elfs = BUILD / "vmelf"
    subprocess.run(["python3", str(ROOT / "tests/vmelf.py"), str(elfs)], check=True)
    image = BUILD / "vm-smoke.img"
    shutil.copyfile(ROOT / "disk.img", image)
    myfs = [str(BUILD / "myfs"), str(image)]
    subprocess.run(myfs + ["rm", "/etc/accounts"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(myfs + ["mkdir", "/tmp"], stdout=subprocess.DEVNULL)
    subprocess.run(myfs + ["put", str(BUILD / "vmtest"), "/bin/vmtest"], check=True)
    subprocess.run(myfs + ["put", str(elfs / "ld.so"), "/tmp/ld.so"], check=True)
    subprocess.run(myfs + ["put", str(elfs / "dyn"), "/tmp/dyn"], check=True)
    run(image, ["vmtest", "vmtest /tmp/dyn", "echo shell-ok"],
        ["cow: ok", "mmap: ok", "file: ok", "vmtest: 0 failed", "ld ok", "dyn ok", "shell-ok"])


if __name__ == "__main__":
    main()
