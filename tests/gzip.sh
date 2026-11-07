#!/bin/sh
# Сверка системного gzip с GNU gzip и с разбором заголовка. Аргумент — собранный бинарник.
set -u
G=${1:?need gzip binary}
case "$G" in
    /*) ;;
    *) G=$(pwd)/$G ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1

printf 'hello gzip\n' > a.txt
"$G" -k -6 a.txt || fail compress
gzip -dc a.txt.gz > a.out || fail gnu-read
cmp a.txt a.out || fail roundtrip
python3 - << 'PY' || fail name-flag
b=open("a.txt.gz","rb").read()
assert b[0:3] == b"\x1f\x8b\x08"
assert b[3] & 8
assert b[10:16] == b"a.txt\0"
PY

gzip -c -9 a.txt > gnu.gz
"$G" -d -k gnu.gz || fail read-gnu
cmp a.txt gnu || fail read-gnu-bytes

cp a.txt.gz named.gz
"$G" -n -f -k a.txt || fail noname
python3 -c 'b=open("a.txt.gz","rb").read(); assert (b[3] & 8)==0' || fail noname-flag
rm -f a.txt
"$G" -d -N named.gz || fail restore
test -f a.txt || fail restored-name
printf 'hello gzip\n' > expect.txt
cmp a.txt expect.txt || fail restored-bytes

python3 - << 'PY' || fail craft
import zlib, struct, binascii
def raw(data, level=9):
    return zlib.compress(data, level)[2:-4]
data = b"extra-field-and-comment"
extra = b"abXY"; comment = b"note"
hdr = bytes([0x1f,0x8b,8,4|16,0,0,0,0,0,255]) + struct.pack("<H", len(extra)) + extra + comment + b"\0"
open("extra.gz","wb").write(hdr + raw(data) + struct.pack("<II", binascii.crc32(data)&0xffffffff, len(data)))
data = b"hdrcrc"
hdr = bytes([0x1f,0x8b,8,2,0,0,0,0,0,255])
open("hcrc.gz","wb").write(hdr + struct.pack("<H", binascii.crc32(hdr)&0xffff) + raw(data) + struct.pack("<II", binascii.crc32(data)&0xffffffff, len(data)))
PY
"$G" -d -k extra.gz || fail extra
printf 'extra-field-and-comment' > expect.txt
cmp extra expect.txt || fail extra-bytes
"$G" -d -k hcrc.gz || fail hcrc
printf 'hdrcrc' > expect.txt
cmp hcrc expect.txt || fail hcrc-bytes

gzip -c a.txt > p1.gz
printf abc > c.txt
gzip -c c.txt > p2.gz
cat p1.gz p2.gz > both.gz
"$G" -d -k both.gz || fail concat
python3 -c 'assert open("both","rb").read()==open("a.txt","rb").read()+b"abc"' || fail concat-bytes

python3 -c 'b=bytearray(open("p1.gz","rb").read()); b[min(20,len(b)-1)]^=0xff; open("bad.gz","wb").write(b)'
"$G" -t bad.gz && fail corrupt || true
"$G" -t p1.gz || fail test
"$G" -l p1.gz | grep -q a.txt || fail list

printf 'z\n' > s.txt
"$G" -S .zz -k s.txt || fail suffix
rm -f s.txt
"$G" -d -S .zz s.txt.zz || fail unsuffix
printf 'z\n' > expect.txt
cmp s.txt expect.txt || fail suffix-bytes
printf 'z\n' > again.txt
"$G" -k again.txt || fail base
"$G" again.txt && fail 'refused existing' || true
"$G" -f again.txt || fail force-overwrite

mkdir -p sub/n
printf 'x\n' > sub/n/f
printf 'plain\n' > sub/n/plain
"$G" -r sub || fail recur
test -f sub/n/f.gz || fail recur-file
test ! -f sub/n/f || fail recur-removed
test -f sub/n/plain.gz || fail recur-plain
"$G" -dr sub || fail unrecur
test ! -f sub/n/f.gz || fail removed-gz
printf 'x\n' > expect.txt
cmp sub/n/f expect.txt || fail unrecur-bytes
printf 'plain\n' > expect.txt
cmp sub/n/plain expect.txt || fail unrecur-plain

"$G" missing-file && fail missing || true
"$G" && fail no-files || true
"$G" -c a.txt && fail stdout || true
"$G" -q sub >/tmp/gztest.out 2>/tmp/gztest.err || fail quiet-rc
test ! -s /tmp/gztest.err || fail quiet-msg

python3 -c 'open("z","wb").write(b"a"*100000); open("o","wb").write(bytes([i%251 for i in range(50000)]))'
"$G" -1 -k z || fail fast
gzip -dc z.gz > z.out || fail fast-gnu
cmp z z.out || fail fast-bytes
"$G" -9 -k o || fail best
gzip -dc o.gz > o.out || fail best-gnu
cmp o o.out || fail best-bytes

cp "$G" ./gunzip
gzip -c a.txt > t.gz
./gunzip -k t.gz || fail gunzip
cmp t a.txt || fail gunzip-bytes
"$G" --best --keep --force z || fail longopt
"$G" -a -q --rsyncable -f -k z || fail ignored

echo "gzip: OK"
rm -rf "$D"
