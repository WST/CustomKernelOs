#!/usr/bin/env python3
"""Собрать ET_DYN из сырого кода nasm: dyn и ld.so с PT_INTERP."""
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent


def elf(code, interp):
    phnum = 2 if interp else 1
    interp_b = (interp.encode() + b"\0") if interp else b""
    ph_bytes = phnum * 32
    interp_off = 52 + ph_bytes
    entry = interp_off + len(interp_b)
    total = entry + len(code)
    load = struct.pack("<IIIIIIII", 1, 0, 0, 0, total, total, 5, 0x1000)
    ph = load
    if interp:
        ph += struct.pack("<IIIIIIII", 3, interp_off, interp_off, interp_off,
                          len(interp_b), len(interp_b), 4, 1)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + b"\0" * 9
    # type, machine, version, entry, phoff, shoff, flags, ehsize, phentsize, phnum, shentsize, shnum, shstrndx
    eh = struct.pack("<HHIIIIIHHHHHH", 3, 3, 1, entry, 52, 0, 0, 52, 32, phnum, 0, 0, 0)
    blob = ident + eh + ph + interp_b + code
    if len(ident + eh) != 52 or len(blob) != total:
        raise SystemExit("bad elf layout %d %d" % (len(ident + eh), len(blob)))
    return blob


def nasm(name: str) -> bytes:
    src = ROOT / "vm" / name
    out = pathlib.Path("/tmp") / (name + ".bin")
    subprocess.run(["nasm", "-f", "bin", "-o", str(out), str(src)], check=True)
    return out.read_bytes()


def main() -> None:
    dest = pathlib.Path(sys.argv[1])
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "ld.so").write_bytes(elf(nasm("ld.asm"), None))
    (dest / "dyn").write_bytes(elf(nasm("dyn.asm"), "/tmp/ld.so"))


if __name__ == "__main__":
    main()
