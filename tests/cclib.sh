#!/bin/sh
# Статические библиотеки cc на хосте: ar и ranlib, архивы от binutils, -l и -L,
# ленивое подтягивание членов, замена библиотечных функций своими, сборка без .a.
# Аргументы: собранный cc-host и каталог с include/ и lib/ (как у make cctest).
set -u
CC=$(realpath "${1:?need cc-host}")
ROOT=$(realpath "${2:?need ccroot}")
TOP=$(cd "$(dirname "$0")/.." && pwd)
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
C="$CC -B $ROOT -linux"
# программа должна напечатать ровно $2
expect() { out=$("./$1") || fail "$1 (exit code)"; [ "$out" = "$2" ] || fail "$1: got '$out', want '$2'"; }
cd "$D" || exit 1

[ -f "$ROOT/lib/libc.a" ] || fail "no $ROOT/lib/libc.a"
$C "$TOP/user/ar.c" -o ar || fail ar-build
ln -s ar ranlib

# библиотека demo: value() зовёт hook(); у hook есть запасное определение в другом члене
cat > value.c <<'EOF'
int hook(void);
int value(void) { return hook() * 10; }
EOF
cat > hook.c <<'EOF'
int hook(void) { return 1; }
EOF
# член, которому не хватает символа: в программу он попадать не должен
cat > broken.c <<'EOF'
int nowhere(void);
int broken(void) { return nowhere(); }
EOF
# цепочка между библиотеками: tick (libp) -> tock (libq) -> tick2 (libp)
printf 'int tock(void);\nint tick(void) { return tock() + 1; }\n' > tick.c
printf 'int tick2(void) { return 100; }\n' > tick2.c
printf 'int tick2(void);\nint tock(void) { return tick2() + 10; }\n' > tock.c
$C -c value.c hook.c broken.c tick.c tick2.c tock.c || fail compile-members

./ar rcs libdemo.a value.o hook.o broken.o || fail ar-rcs
[ "$(./ar t libdemo.a | tr '\n' ' ')" = "value.o hook.o broken.o " ] || fail ar-t
./ar rcs libp.a tick.o tick2.o && ./ar rcs libq.a tock.o || fail ar-chain

cat > use.c <<'EOF'
#include <stdio.h>
int value(void);
int main(void) { printf("%d\n", value()); return 0; }
EOF
cat > own.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
int value(void);
int hook(void) { return 7; }
int rand(void) { return 4; }
int main(void) { printf("%d %d\n", value(), rand()); return 0; }
EOF
printf '#include <stdio.h>\nint tick(void);\nint main(void) { printf("%%d\\n", tick()); return 0; }\n' > chain.c

$C use.c libdemo.a -o use || fail lib-file
expect use 10
$C libdemo.a use.c -o use-first || fail lib-before-source
expect use-first 10
$C use.c -L. -ldemo -o use-l || fail -l
expect use-l 10
$C use.c -L . -l demo -o use-l2 || fail "-L dir -l name"
expect use-l2 10
$C -c use.c && $C use.o -L. -ldemo -o use-o || fail object-with-lib
expect use-o 10
$C own.c -L. -ldemo -o own || fail override
expect own "70 4"
$C chain.c -L. -lq -lp -o chain || fail chain
expect chain 111
$C use.c -lnope -o x 2> nope.err && fail missing-lib-accepted
grep -q "cannot find -lnope" nope.err || { cat nope.err; fail missing-lib-message; }
printf 'int broken(void);\nint main(void) { return broken(); }\n' > usebroken.c
$C usebroken.c -L. -ldemo -o x 2> broken.err && fail broken-member-accepted
grep -q "undefined reference to 'nowhere' in broken (.*libdemo.a(broken.o))" broken.err || { cat broken.err; fail broken-member-message; }

# ranlib: архив без индекса (S) работает и так, а ranlib добавляет индекс
./ar rcS libnoidx.a value.o hook.o || fail ar-S
$C use.c libnoidx.a -o use-noidx || fail no-index
expect use-noidx 10
./ranlib libnoidx.a || fail ranlib
if command -v nm >/dev/null 2>&1; then
    nm --print-armap libnoidx.a 2>/dev/null | grep -q "value in value.o" || fail ranlib-index
fi

# архивы binutils и наш ar в binutils
if command ar --version 2>/dev/null | grep -q GNU; then
    rm -f libgnu.a && command ar rcs libgnu.a value.o hook.o broken.o || fail gnu-ar
    $C use.c libgnu.a -o use-gnu || fail gnu-archive
    expect use-gnu 10
    [ "$(command ar t libdemo.a | tr '\n' ' ')" = "value.o hook.o broken.o " ] || fail gnu-reads-ours
    # с нулевыми датами и владельцами (D) архивы совпадают байт в байт
    cp hook.o a_member_with_a_long_name.o
    rm -f gnu-d.a ours-d.a && command ar rcsD gnu-d.a value.o a_member_with_a_long_name.o broken.o && \
        ./ar rcs ours-d.a value.o a_member_with_a_long_name.o broken.o || fail ar-long-names
    cmp -s gnu-d.a ours-d.a || fail "ar output differs from GNU ar rcsD"
fi
if gcc -m32 -O2 -fno-pic -fno-pie -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
       -mno-sse -mno-mmx -c hook.c -o hook-gcc.o 2>/dev/null; then
    ./ar rcs libgcc-hook.a hook-gcc.o && $C use.c value.o libgcc-hook.a -o use-gcc || fail gcc-member
    expect use-gcc 10
fi

# нет libc.a и libgfx.a — библиотека компилируется из исходников, как раньше
mkdir src-root && cp -r "$ROOT/include" "$ROOT/lib" src-root/ && rm -f src-root/lib/*.a
$CC -B src-root -linux own.c -L. -ldemo -o own-src || fail source-libc
expect own-src "70 4"
$CC -B src-root -linux use.c -o x 2> src.err && fail source-undefined-accepted
$CC -B src-root -linux "$TOP/tests/cc/test1.c" -o test1-src || fail source-test1
./test1-src > test1.out || { cat test1.out; fail source-test1-run; }

echo "cc static libraries: OK"
