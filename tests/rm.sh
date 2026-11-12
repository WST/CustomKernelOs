#!/bin/sh
# rm на хосте через cc -linux. Аргумент — собранный бинарник.
set -u
R=${1:?need rm binary}
case "$R" in
    /*) ;;
    *) R=$(pwd)/$R ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1

"$R" -f || fail force-none
"$R" -rf || fail rf-none
"$R" && fail noarg || true

printf 'z\n' > file
"$R" file || fail file
[ ! -e file ] || fail file-left

mkdir empty
"$R" empty || fail empty
[ ! -e empty ] || fail empty-left

mkdir -p full/x
printf 'q\n' > full/x/q
"$R" full && fail nonempty || true
[ -f full/x/q ] || fail nonempty-gone
"$R" -f full && fail force-nonempty || true
[ -f full/x/q ] || fail force-nonempty-gone
"$R" -r full || fail recur
[ ! -e full ] || fail recur-left

mkdir -p tree/a/b tmp/ext/deep
printf 'c\n' > tree/a/b/c
printf 'h\n' > tree/.hid
printf 'e\n' > tmp/ext/deep/e
ln -s tree/a linkdir
"$R" -rf linkdir || fail symlink
[ ! -e linkdir ] || fail symlink-left
[ -f tree/a/b/c ] || fail symlink-followed
# Trailing separators must not turn a symlink into recursive target deletion.
ln -s tree/a trailing
"$R" -rf trailing/// || fail trailing-symlink
[ ! -L trailing ] || fail trailing-symlink-left
[ -f tree/a/b/c ] || fail trailing-symlink-followed
mkdir -p broken-target
ln -s absent dangling
"$R" -rf dangling/ || fail dangling-symlink
[ ! -L dangling ] || fail dangling-symlink-left
"$R" -rf broken-target/// || fail trailing-directory
[ ! -e broken-target ] || fail trailing-directory-left
"$R" -fr tree || fail fr
[ ! -e tree ] || fail fr-left
"$R" --recursive --force tmp/ext || fail long
[ ! -e tmp/ext ] || fail long-left
"$R" -rf tmp/ext || fail missing-force
"$R" tmp/ext && fail missing || true

mkdir -p box
printf 'keep\n' > box/keep
(
    cd box || exit 1
    "$R" -rf . && exit 2
    [ -f keep ] || exit 3
) || fail dot
[ -f box/keep ] || fail dot-left
"$R" -rf -- box || fail dashdash
[ ! -e box ] || fail dashdash-left

"$R" -x && fail badopt || true

echo "rm: OK"
rm -rf "$D"
