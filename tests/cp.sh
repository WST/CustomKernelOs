#!/bin/sh
# cp на хосте через cc -linux. Аргумент — собранный бинарник.
set -u
G=${1:?need cp binary}
case "$G" in
    /*) ;;
    *) G=$(pwd)/$G ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1

printf 'hello\n' > a.txt
"$G" a.txt b.txt || fail file
cmp a.txt b.txt || fail file-bytes
printf 'other\n' > b.txt
"$G" a.txt b.txt || fail overwrite
cmp a.txt b.txt || fail overwrite-bytes
"$G" a.txt a.txt || fail same
printf 'hello\n' > expect.txt
cmp a.txt expect.txt || fail same-bytes

mkdir box
"$G" a.txt box || fail into-dir
cmp a.txt box/a.txt || fail into-dir-bytes
"$G" a.txt b.txt box || fail multi
cmp box/b.txt a.txt || fail multi-bytes
"$G" a.txt b.txt missing && fail multi-nodir || true

mkdir -p src/sub
printf 'x\n' > src/f
printf 'y\n' > src/sub/g
"$G" src dst && fail no-r || true
"$G" -r src dst || fail tree
cmp src/f dst/f || fail tree-f
cmp src/sub/g dst/sub/g || fail tree-g
"$G" -r src dst || fail into-tree
cmp dst/src/f src/f || fail into-tree-f
"$G" -rvf src/f dst/f >verb.out 2>verb.err || fail verbose
grep -q 'src/f -> dst/f' verb.err || fail verbose-msg

echo "cp: OK"
rm -rf "$D"
