#!/bin/sh
# ls на хосте через cc -linux. Аргумент — собранный бинарник.
set -u
L=${1:?need ls binary}
case "$L" in
    /*) ;;
    *) L=$(pwd)/$L ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1
mkdir work
cd work || exit 1

mkdir -p sub/nested
printf 'vis\n' > visible
printf 'hid\n' > .hidden
printf 'nest\n' > sub/nested/f
dd if=/dev/zero of=big bs=1 count=1536 status=none
dd if=/dev/zero of=exact bs=1 count=1024 status=none
dd if=/dev/zero of=ten bs=1 count=10240 status=none
cd "$D" || exit 1

"$L" -lha work >lha.out || fail lha
grep -q '\.hidden' lha.out || fail lha-hidden
grep -q '1\.5K' lha.out || fail lha-human
grep -q '1\.0K' lha.out || fail lha-exact
grep -q '10K' lha.out || fail lha-ten
grep -E '^d.* \.$' lha.out >/dev/null || fail lha-dot
grep -E '^d.* \.\.$' lha.out >/dev/null || fail lha-dotdot
grep -q visible lha.out || fail lha-visible

# Размер «..» — это родитель, он растёт от файлов с выводом. Его не сравниваем.
strip_dotdot() { grep -v ' \.\.$' "$1"; }
"$L" -l -h -a work >split.out || fail split
strip_dotdot lha.out >lha.cmp
strip_dotdot split.out >split.cmp
cmp lha.cmp split.cmp || fail split-same
"$L" -lah work >lah.out || fail lah
strip_dotdot lah.out >lah.cmp
cmp lha.cmp lah.cmp || fail lah-same
"$L" -l --all --human-readable work >long.out || fail longopt
strip_dotdot long.out >long.cmp
cmp lha.cmp long.cmp || fail longopt-same

"$L" work >plain.out || fail plain
grep -q '\.hidden' plain.out && fail plain-shows-hidden || true
grep -q visible plain.out || fail plain-visible

"$L" -lA work >almost.out || fail almost
grep -q '\.hidden' almost.out || fail almost-hidden
grep -E '^d.* \.$' almost.out >/dev/null && fail almost-dot || true
grep -E '^d.* \.\.$' almost.out >/dev/null && fail almost-dotdot || true

"$L" -l work/big >num.out || fail numeric
grep -q '1536' num.out || fail numeric-size
grep -q '1\.5K' num.out && fail numeric-human || true

"$L" -lh work/big >one.out || fail one-human
grep -q '1\.5K' one.out || fail one-human-size
grep -q '^-' one.out || fail one-mode

"$L" -lha work/sub >sub.out || fail sub
grep -q nested sub.out || fail sub-nested
grep -q '1\.5K' sub.out && fail sub-leak || true

"$L" -z >bad.out 2>bad.err && fail badopt || true
grep -q "unknown option" bad.err || fail badopt-msg
"$L" no-such >miss.out 2>miss.err && fail missing || true
grep -q 'no-such' miss.err || fail missing-msg

echo "ls: OK"
rm -rf "$D"
