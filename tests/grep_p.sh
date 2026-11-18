#!/bin/sh
# grep -P зовёт libpcre; без флага остаётся прежний разбор.
set -u
G=${1:?need grep}
case "$G" in
    /*) ;;
    *) G=$(pwd)/$G ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1
printf 'abc\n123\nXYZ\n' > in.txt
"$G" -P '^\d+' in.txt > p.out || fail p-status
printf '123\n' > p.exp
cmp p.out p.exp || fail p-bytes
"$G" -P -i 'xyz' in.txt > i.out || fail i-status
printf 'XYZ\n' > i.exp
cmp i.out i.exp || fail i-bytes
"$G" '1.3' in.txt > old.out || fail old-status
cmp old.out p.exp || fail old-bytes
echo "grep -P ok"
rm -rf "$D"
