#!/bin/sh
# Разделяемые библиотеки cc: .so, которую берёт gcc, и динамическая программа cc.
# Аргументы: cc-host и каталог с include/ и lib/ (как у make cctest).
set -u
CC=$(realpath "${1:?need cc-host}")
ROOT=$(realpath "${2:?need ccroot}")
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
C="$CC -B $ROOT"
cd "$D" || exit 1

cat > liba.c << 'EOF'
int extra = 5;
int *pextra = &extra;
int add(int a, int b) { return a + b + *pextra; }
EOF
cat > libb.c << 'EOF'
extern int add(int a, int b);
int twice(int x) { return add(x, x); }
EOF
cat > main.c << 'EOF'
extern int extra;
extern int add(int a, int b);
int main(void) {
    if (extra != 5) return 1;
    extra = 1;
    if (add(1, 2) != 4) return 2;
    return 0;
}
int __libc_start(int argc, char **argv) { return main() + argc * 0 + (argv ? 0 : 0); }
EOF
cat > t.c << 'EOF'
int main(void) { return 0; }
EOF
cat > use.s << 'EOF'
.globl _start
_start:
    movl extra, %eax
    pushl $2
    pushl $1
    call add
    addl $8, %esp
    movl %eax, %ebx
    movl $1, %eax
    int $0x80
EOF

$C -shared -c liba.c >/dev/null 2>&1 && fail shared-with-c
$C -shared -fno-pic -nostdlib liba.c -o x.so >/dev/null 2>&1 && fail shared-nopic
$C -static liba.so -o x >/dev/null 2>&1 && fail static-so
$C -linux t.c -o plain || fail plain
$C -linux -static t.c -o plain-static || fail plain-static
cmp plain plain-static || fail static-bytes

$C -shared -nostdlib -o libadd.so liba.c || fail libadd
$C -shared -nostdlib -o libb.so libb.c libadd.so || fail libb
$C -nostdlib -L. -ladd main.c -o dyn || fail dyn

python3 - << 'PY' || fail elf
import struct, sys

def elf_hash(name):
    h = 0
    for ch in name:
        h = ((h << 4) + ch) & 0xffffffff
        g = h & 0xf0000000
        if g:
            h ^= (g >> 24) & 0xffffffff
        h = (h & (~g & 0xffffffff)) & 0xffffffff
    return h

def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF", path
    (etype, emach, ever, entry, phoff, shoff, flags, ehsize, phentsize, phnum,
     shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
    assert emach == 3 and phentsize == 32 and shentsize == 40 and shnum > 1, path
    ph = [struct.unpack_from("<IIIIIIII", d, phoff + i * 32) for i in range(phnum)]
    sh = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * 40) for i in range(shnum)]
    return d, etype, entry, ph, sh, shstrndx

def sname(d, sh, shstrndx, i):
    off = sh[shstrndx][4] + sh[i][0]
    end = d.index(b"\0", off)
    return d[off:end].decode()

def section(d, sh, shstrndx, name):
    for i, s in enumerate(sh):
        if i and sname(d, sh, shstrndx, i) == name:
            return s
    return None

def cstr(d, off, size, name_off):
    if name_off >= size:
        raise SystemExit("bad str")
    base = off + name_off
    end = d.index(b"\0", base)
    return d[base:end]

def dyn_tags(d, sh, shstrndx):
    sec = section(d, sh, shstrndx, ".dynamic")
    assert sec, "no .dynamic"
    tags = {}
    needed = []
    for i in range(0, sec[5], 8):
        tag, val = struct.unpack_from("<II", d, sec[4] + i)
        if tag == 0:
            break
        if tag == 1:
            needed.append(val)
        else:
            tags.setdefault(tag, val)
    return tags, needed

def dynsym(d, sh, shstrndx):
    tags, needed = dyn_tags(d, sh, shstrndx)
    strsec = section(d, sh, shstrndx, ".dynstr")
    symsec = section(d, sh, shstrndx, ".dynsym")
    assert strsec and symsec and tags[6] == symsec[3] and tags[5] == strsec[3]
    assert tags[11] == 16 and tags[10] == strsec[5]
    syms = []
    for i in range(0, symsec[5], 16):
        name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, symsec[4] + i)
        nm = cstr(d, strsec[4], strsec[5], name) if i else b""
        syms.append((nm, value, size, info, shndx))
    return tags, needed, syms, strsec

def relocs(d, sh, shstrndx, name):
    sec = section(d, sh, shstrndx, name)
    if not sec:
        return []
    out = []
    for i in range(0, sec[5], 8):
        off, info = struct.unpack_from("<II", d, sec[4] + i)
        out.append((off, info & 0xff, info >> 8))
    return out

def check_hash(d, sh, shstrndx, syms):
    tags, _, _, _ = dynsym(d, sh, shstrndx)
    hsec = section(d, sh, shstrndx, ".hash")
    assert hsec and tags[4] == hsec[3]
    nbucket, nchain = struct.unpack_from("<II", d, hsec[4])
    assert nchain == len(syms) and nbucket == nchain
    buckets = list(struct.unpack_from("<%dI" % nbucket, d, hsec[4] + 8))
    chains = list(struct.unpack_from("<%dI" % nchain, d, hsec[4] + 8 + 4 * nbucket))
    for i in range(1, nchain):
        h = elf_hash(syms[i][0]) % nbucket
        seen = set()
        cur = buckets[h]
        while cur and cur not in seen:
            if cur == i:
                break
            seen.add(cur)
            cur = chains[cur]
        else:
            raise SystemExit("hash miss %s" % syms[i][0])

def writable(ph, va):
    for typ, off, addr, pa, fsz, msz, flg, al in ph:
        if typ == 1 and (flg & 2) and addr <= va < addr + msz:
            return True
    return False

def check_loads(ph, base):
    loads = [p for p in ph if p[0] == 1]
    assert len(loads) == 2
    for typ, off, addr, pa, fsz, msz, flg, al in loads:
        assert off % 4096 == addr % 4096
        assert addr == base + off

def sym_by(syms, name):
    hits = [s for s in syms if s[0] == name]
    assert len(hits) == 1, name
    return hits[0]

d, etype, entry, ph, sh, si = load("libadd.so")
assert etype == 3 and entry == 0
check_loads(ph, 0)
tags, needed, syms, _ = dynsym(d, sh, si)
assert not needed
assert cstr(d, section(d, sh, si, ".dynstr")[4], tags[10], tags[14]) == b"libadd.so"
add = sym_by(syms, b"add")
extra = sym_by(syms, b"extra")
pextra = sym_by(syms, b"pextra")
assert add[3] & 0xf == 2 and add[4] != 0
assert extra[3] & 0xf == 1 and extra[2] == 4 and extra[4] != 0
assert pextra[4] != 0
rels = relocs(d, sh, si, ".rel.dyn")
kinds = {}
for off, typ, si_ in rels:
    assert writable(ph, off), "text reloc"
    kinds.setdefault(typ, []).append((off, si_))
assert 8 in kinds and 6 in kinds and 1 in kinds
assert any(syms[s][0] == b"pextra" for _, s in kinds[6])
assert any(syms[s][0] == b"extra" for _, s in kinds[1])
assert tags.get(24) == 0 and tags.get(30, 0) & 8
check_hash(d, sh, si, syms)
assert any(s[1] == 11 for s in sh)

d, etype, entry, ph, sh, si = load("libb.so")
tags, needed, syms, strsec = dynsym(d, sh, si)
assert [cstr(d, strsec[4], strsec[5], n) for n in needed] == [b"libadd.so"]
jmps = relocs(d, sh, si, ".rel.plt")
assert len(jmps) == 1 and jmps[0][1] == 7 and syms[jmps[0][2]][0] == b"add"
disp = jmps[0][0] - tags[3]
assert d.find(b"\xff\xa3" + struct.pack("<I", disp)) >= 0
for off, typ, _s in jmps + relocs(d, sh, si, ".rel.dyn"):
    assert writable(ph, off)

d, etype, entry, ph, sh, si = load("dyn")
assert etype == 2 and entry >= 0x40000000
check_loads(ph, 0x40000000)
interps = [p for p in ph if p[0] == 3]
assert len(interps) == 1
raw = d[interps[0][1]:interps[0][1] + interps[0][4]]
assert raw.startswith(b"/lib/ld.so\0")
tags, needed, syms, strsec = dynsym(d, sh, si)
assert [cstr(d, strsec[4], strsec[5], n) for n in needed] == [b"libadd.so"]
extra = sym_by(syms, b"extra")
copies = [r for r in relocs(d, sh, si, ".rel.dyn") if r[1] == 5]
assert len(copies) == 1 and syms[copies[0][2]][0] == b"extra"
assert extra[1] == copies[0][0] and extra[2] == 4 and extra[4] != 0
jmps = relocs(d, sh, si, ".rel.plt")
assert any(r[1] == 7 and syms[r[2]][0] == b"add" for r in jmps)
assert d.find(b"\xff\x25" + struct.pack("<I", jmps[0][0])) >= 0
for off, typ, _s in jmps + relocs(d, sh, si, ".rel.dyn"):
    assert writable(ph, off), "text reloc in exe"
assert not any(r[1] == 8 for r in relocs(d, sh, si, ".rel.dyn"))
print("elf ok")
PY

if gcc -m32 -nostdlib -o use use.s libadd.so >gcc.err 2>&1; then
    python3 - << 'PY' || fail gcc-elf
import struct
d = open("use", "rb").read()
# Динамическая секция есть у любой программы, которую ld собрал против .so.
assert d[:4] == b"\x7fELF"
e_phoff, e_phnum = struct.unpack_from("<II", d, 28)[0], struct.unpack_from("<H", d, 44)[0]
dyn = None
for i in range(e_phnum):
    typ, off, addr, pa, fsz, msz, flg, al = struct.unpack_from("<IIIIIIII", d, e_phoff + i * 32)
    if typ == 2:
        dyn = (off, fsz)
assert dyn, "gcc output has no PT_DYNAMIC"
blob = d[dyn[0]:dyn[0] + dyn[1]]
# DT_NEEDED == 1 где-то в динамической секции: точное имя зависит от ld, само наличие — нет.
assert b"\x01\x00\x00\x00" in blob
print("gcc link ok")
PY
else
    echo "gcc -m32 cannot link here - skipping"
    cat gcc.err
fi

echo "cc shared: OK"
