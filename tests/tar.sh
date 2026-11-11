#!/bin/sh
# Сверка tar с GNU tar: ustar, GNU long name, pax, gzip и настоящий архив Lua.
set -u
T=${1:?need tar binary}
case "$T" in
    /*) ;;
    *) T=$(pwd)/$T ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1

python3 - << 'PY' || fail craft
import tarfile, gzip, os
os.makedirs("src/sub", exist_ok=True)
open("src/hello.txt","w").write("hello tar\n")
open("src/sub/deep.txt","w").write("deep\n")
long = "n" * 120 + ".txt"
open("src/" + long, "w").write("long-name\n")
os.symlink("hello.txt", "src/link")
# ustar: каталог, файл, ссылка
with tarfile.open("plain.tar", "w", format=tarfile.USTAR_FORMAT) as t:
    t.add("src/hello.txt", arcname="pkg/hello.txt")
    t.add("src/sub", arcname="pkg/sub")
    t.add("src/link", arcname="pkg/link")
# GNU: длинное имя
with tarfile.open("gnu.tar", "w", format=tarfile.GNU_FORMAT) as t:
    t.add("src/" + long, arcname="pkg/" + long)
    t.add("src/hello.txt", arcname="pkg/hello.txt")
# pax: длинное имя и обычный файл
with tarfile.open("pax.tar", "w", format=tarfile.PAX_FORMAT) as t:
    t.add("src/" + long, arcname="pkg/" + long)
    t.add("src/hello.txt", arcname="pkg/hello.txt")
raw = open("plain.tar","rb").read()
with gzip.open("plain.tar.gz", "wb") as g:
    g.write(raw)
open("longname","w").write(long)
PY

extract() {
    rm -rf "$1"
    mkdir "$1"
    "$T" -xf "$2" -C "$1" || fail "extract $2"
}

extract ours plain.tar
mkdir gnu-plain
tar -xf plain.tar -C gnu-plain || fail gnu-plain
diff -rq ours gnu-plain || fail plain-diff
test -f ours/pkg/hello.txt || fail hello
test -L ours/pkg/link || fail symlink
readlink ours/pkg/link | grep -qx hello.txt || fail link-text
cmp ours/pkg/hello.txt src/hello.txt || fail hello-bytes

extract gz plain.tar.gz
diff -rq gz ours || fail gzip-auto
rm -rf gz-z
mkdir gz-z
"$T" -xzf plain.tar.gz -C gz-z || fail gzip-flag
diff -rq gz-z ours || fail gzip-flag-diff

# традиционный синтаксис без дефисов и --strip-components
rm -rf strip
mkdir strip
"$T" xzf plain.tar.gz -C strip --strip-components=1 || fail strip
test -f strip/hello.txt || fail strip-file
test ! -d strip/pkg || fail strip-left

extract gnu gnu.tar
mkdir gnu-ref
tar -xf gnu.tar -C gnu-ref || fail gnu-ref
diff -rq gnu gnu-ref || fail gnu-diff
LONG=$(cat longname)
test -f "gnu/pkg/$LONG" || fail gnu-long
cmp "gnu/pkg/$LONG" "src/$LONG" || fail gnu-long-bytes

extract pax pax.tar
mkdir pax-ref
tar -xf pax.tar -C pax-ref || fail pax-ref
diff -rq pax pax-ref || fail pax-diff

# список имён
"$T" -tf plain.tar | sort > list.ours
tar -tf plain.tar | sed 's|/$||' | sort > list.gnu
diff -u list.ours list.gnu || fail list

# член архива и -k
rm -rf one
mkdir one
"$T" -xf plain.tar -C one pkg/hello.txt || fail member
test -f one/pkg/hello.txt || fail member-file
test ! -e one/pkg/link || fail member-extra
printf 'keep\n' > one/pkg/hello.txt
"$T" -xkf plain.tar -C one || fail keep
cmp one/pkg/hello.txt - << 'EOF' || fail keep-bytes
keep
EOF
test -L one/pkg/link || fail keep-added-link

# «..» не выходит из каталога
python3 - << 'PY' || fail dotdot-craft
import tarfile
open("inside.txt","w").write("nope\n")
with tarfile.open("dot.tar","w", format=tarfile.USTAR_FORMAT) as t:
    t.add("inside.txt", arcname="ok/../../escaped.txt")
    t.add("inside.txt", arcname="ok/stay.txt")
PY
rm -rf dot
mkdir dot
"$T" -xf dot.tar -C dot; status=$?
test "$status" != 0 || fail dotdot-status
test ! -f escaped.txt || fail escaped
test ! -f dot/escaped.txt || fail escaped-in
test -f dot/ok/stay.txt || fail stay

# настоящий архив с исходниками zlib (релиз 1.3.1)
URL=https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
curl -fsSL --max-time 40 -o zlib.tar.gz "$URL" || fail "download zlib"
rm -rf zlib-ours zlib-gnu
mkdir zlib-ours zlib-gnu
"$T" -xzf zlib.tar.gz -C zlib-ours || fail zlib-ours
tar -xzf zlib.tar.gz -C zlib-gnu || fail zlib-gnu
diff -rq zlib-ours zlib-gnu || fail zlib-diff
test -f zlib-ours/zlib-1.3.1/zlib.h || fail zlib-header
"$T" tzf zlib.tar.gz | wc -l > zlib-count.ours
tar tzf zlib.tar.gz | wc -l > zlib-count.gnu
diff -u zlib-count.ours zlib-count.gnu || fail zlib-count

echo "tar tests passed"
rm -rf "$D"
