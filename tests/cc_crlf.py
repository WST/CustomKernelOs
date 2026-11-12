"""Check CRLF macro splicing in source files and included headers."""
import pathlib
import subprocess
import sys
import tempfile
compiler = str(pathlib.Path(sys.argv[1]).resolve())
sysroot = str(pathlib.Path(sys.argv[2]).resolve())
with tempfile.TemporaryDirectory(prefix="myos-cc-crlf-") as name:
    root = pathlib.Path(name)
    (root / "value.h").write_bytes(b"#define VALUE \\\r\n 42\r\n")
    source = root / "source.c"
    source.write_bytes(b'#include "value.h"\r\n#define PLUS(x) \\\r\n ((x)+1)\r\nint main(void){return PLUS(VALUE)!=43;}\r\n')
    for flags in ([], ["-fPIC"]):
        output = root / "test"
        subprocess.run([compiler, "-B", sysroot, "-linux", *flags, str(source), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)
print("CRLF macro splicing: OK")
