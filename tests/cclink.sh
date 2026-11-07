#!/bin/sh
# Раздельная компиляция cc на хосте: cc -c, сборка из .o, .o от gcc, наши .o в binutils.
# Аргументы: собранный cc-host и каталог с include/ и lib/ (как у make cctest).
set -u
CC=$(realpath "${1:?need cc-host}")
ROOT=$(realpath "${2:?need ccroot}")
SRC=$(cd "$(dirname "$0")/cc/link" && pwd)
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
run() { "./$1" > "$1.out" || { cat "$1.out"; fail "$1"; }; }
C="$CC -B $ROOT -linux -I $SRC"
cd "$D" || exit 1

$C "$SRC/main.c" "$SRC/lib1.c" "$SRC/lib2.c" -o all || fail all-build
run all

$C -c "$SRC/main.c" "$SRC/lib1.c" "$SRC/lib2.c" || fail compile-only
[ -f main.o ] && [ -f lib1.o ] && [ -f lib2.o ] || fail object-names
$C main.o lib1.o lib2.o -o separate || fail separate-build
run separate
$C "$SRC/main.c" lib1.o lib2.o -o mixed || fail mixed-build
run mixed

$C -c "$SRC/lib1.c" -o other.o && [ -f other.o ] || fail c-with-o
$C -c "$SRC/lib1.c" "$SRC/lib2.c" -o x.o 2>/dev/null && fail c-with-o-many

# объекты gcc (с .eh_frame, .note.gnu.property и вызовом __divdi3 из нашей libc)
if gcc -m32 -O2 -fno-pic -fno-pie -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
       -mno-sse -mno-mmx -I "$ROOT/include" -c "$SRC/lib2.c" -o lib2-gcc.o 2>gcc.err; then
    $C main.o lib1.o lib2-gcc.o -o with-gcc || fail gcc-build
    run with-gcc
else
    echo "gcc -m32 cannot build objects here - skipping gcc objects"
fi

# binutils понимают наши .o: readelf без предупреждений, ld -r склеивает, cc читает результат
if command -v readelf >/dev/null 2>&1; then
    readelf -W -a main.o > /dev/null 2> readelf.err
    [ -s readelf.err ] && { cat readelf.err; fail readelf; }
fi
if command -v ld >/dev/null 2>&1; then
    ld -m elf_i386 -r main.o lib1.o -o main-lib1.o || fail ld-r
    $C main-lib1.o lib2.o -o via-ld || fail via-ld-build
    run via-ld
fi

printf 'int missing(void);\nint main(void) { return missing(); }\n' > undef.c
$C undef.c -o undef 2> undef.err && fail undefined-accepted
grep -q "undefined reference to 'missing' in main" undef.err || { cat undef.err; fail undefined-message; }
printf 'int lib1_helper(void) { return 0; }\n' > dup.c
$C -c dup.c || fail dup-compile
$C main.o lib1.o lib2.o dup.o -o dup 2> dup.err && fail duplicate-accepted
grep -q "multiple definition of 'lib1_helper'" dup.err || { cat dup.err; fail duplicate-message; }

# -fPIC вместе с обычными объектами: GOT и PLT32, программа та же
P="$C -fPIC"
$P "$SRC/main.c" "$SRC/lib1.c" "$SRC/lib2.c" -o allpic || fail allpic-build
run allpic
$P -c "$SRC/main.c" -o mainpic.o || fail pic-compile
$C -c "$SRC/lib1.c" -o lib1nop.o || fail nopic-compile
$C mainpic.o lib1nop.o lib2.o -o mixpic || fail mixpic-build
run mixpic

echo "cc separate compilation: OK"
