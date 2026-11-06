"""Headless QEMU integration test on disposable raw and partitioned images."""
import pathlib
import re
import shutil
import subprocess
import time

BUILD = pathlib.Path("build")


def clean(text):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text).replace("\r", "")


def run(image, commands, expected, name, data_disk=None):
    log = BUILD / (name + ".serial.log")
    log.write_text("")
    args = [
        "qemu-system-i386", "-m", "256M", "-vga", "vmware", "-display", "none",
        "-drive", "format=raw,file=build/os.img,index=0",
        "-drive", f"format=raw,file={image},index=1",
        "-nic", "user,model=e1000", "-serial", f"file:{log}",
        "-monitor", "stdio", "-no-reboot",
    ]
    if data_disk:
        args += ["-drive", f"format=raw,file={data_disk},index=2"]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 20
        while "sh:/" not in clean(log.read_text(errors="replace")):
            if proc.poll() is not None:
                raise RuntimeError(proc.stderr.read())
            if time.monotonic() > deadline:
                raise RuntimeError("boot timeout: " + log.read_text(errors="replace"))
            time.sleep(.1)
        names = {" ": "spc", "/": "slash", ".": "dot", ">": "shift-dot", "-": "minus"}
        for command in commands:
            for ch in command:
                proc.stdin.write("sendkey " + names.get(ch, ch) + " 10\n")
                proc.stdin.flush()
                time.sleep(.035)
            previous = log.stat().st_size
            proc.stdin.write("sendkey ret 10\n")
            proc.stdin.flush()
            deadline = time.monotonic() + 60
            while True:
                time.sleep(.1)
                text = clean(log.read_text(errors="replace"))
                if log.stat().st_size > previous and re.search(r"(?:^|\n)sh:[^\n]*\$ *$", text):
                    break
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"{name}: command timed out: {command}; see {log}")
        text = clean(log.read_text(errors="replace"))
        for marker in expected:
            assert marker in text, f"{name}: missing {marker!r}; see {log}"
        assert "PANIC" not in text and "kernel stack overflow" not in text, text
        print(name + ": OK")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


shutil.copyfile("disk.img", BUILD / "vfs-raw.img")
run(BUILD / "vfs-raw.img", ["lsblk", "ls /dev", "mount", "echo raw-ok > /tmp/check", "cat /tmp/check"],
    ["/dev/sdb", "<BLOCK>", "<CHAR>", "devfs on /dev", "raw-ok"], "vfs-raw")
second = BUILD / "vfs-second.img"
with second.open("wb") as f:
    f.truncate(2 * 1024 * 1024)
subprocess.run(["build/myfs", str(second), "mkdir", "/data"], check=True, stdout=subprocess.DEVNULL)
partitioned = BUILD / "vfs-partitioned.img"
# Generate uniquely named output: the wrapping tool never overwrites an image.
if partitioned.exists():
    partitioned.unlink()
subprocess.run(["python3", "tools/partition_image.py", str(partitioned), "disk.img", str(second)], check=True)
run(partitioned, ["lsblk", "mount /dev/sdb2 /mnt", "mount", "echo volume-ok > /mnt/hello", "cat /mnt/hello", "cd /mnt", "pwd", "cd ..", "pwd"],
    ["/dev/sdb1", "/dev/sdb2 on /mnt", "volume-ok", "/mnt"], "vfs-partitioned")
run(partitioned, ["mount /dev/sdb2 /mnt", "cat /mnt/hello"], ["volume-ok"], "vfs-reboot")

# Partition and format a separate 1 GiB data disk entirely inside MyOS.
data_disk = BUILD / "vfs-data-1g.img"
with data_disk.open("wb") as f:
    f.truncate(1073741824)
run(BUILD / "vfs-raw.img", [
    "lsblk", "fdisk init /dev/sdc --yes", "fdisk add /dev/sdc 1 1 512",
    "fdisk add /dev/sdc 2 513 511", "fdisk /dev/sdc",
    "mkfs /dev/sdc1 --yes", "mkfs /dev/sdc2 --yes", "mount /dev/sdc1 /mnt",
    "echo data-ok > /mnt/persist", "cat /mnt/persist",
    "mkfs /dev/sdb --yes", "fdisk init /dev/sdb --yes",
    "fdisk del /dev/sdc 2 --yes",
], ["size=512 MiB", "size=511 MiB", "MyFS created on /dev/sdc1", "MyFS created on /dev/sdc2", "\ndata-ok\n", "mountpoint or device busy"], "vfs-data-1g", data_disk)
# Deletion on a disk with any mounted partition is refused. Both volumes survive.
run(BUILD / "vfs-raw.img", ["lsblk", "mount /dev/sdc1 /mnt", "cat /mnt/persist", "mkdir /media/check", "mount /dev/sdc2 /media/check", "mount"],
    ["\ndata-ok\n", "/dev/sdc2 on /media/check"], "vfs-data-reboot", data_disk)
