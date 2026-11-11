#!/bin/sh
# Сборка Lua 5.4.8 тем же cc, что стоит в системе, и короткая проверка языка.
set -e
CC=$1
ROOT=$2
if [ -z "$CC" ] || [ -z "$ROOT" ]; then
    echo "usage: tests/lua.sh build/cc-host build/ccroot" >&2
    exit 2
fi

inst=ext/instructions/lua.inst
link=$(grep '^cc -o /ext/bin/lua ' "$inst")
words=$(printf '%s\n' "$link" | wc -w)
bytes=$(printf '%s' "$link" | wc -c)
if [ "$words" -ge 64 ] || [ "$bytes" -ge 1024 ]; then
    echo "lua.inst link line is too long: $words words, $bytes bytes" >&2
    exit 1
fi

src=build/lua-5.4.8
if [ ! -f "$src/lua.h" ]; then
    mkdir -p build
    curl -fsSL -o build/lua-5.4.8.tar.gz https://github.com/lua/lua/archive/refs/tags/v5.4.8.tar.gz
    tar -C build -xzf build/lua-5.4.8.tar.gz
fi

mkdir -p build/luaobj
objs=
for f in "$src"/*.c; do
    base=$(basename "$f" .c)
    case $base in
        ltests|luac|onelua) continue ;;
    esac
    echo "cc $base.c"
    "$CC" -B "$ROOT" -linux -DLUA_USE_C89 -c "$f" -o "build/luaobj/$base.o"
    objs="$objs build/luaobj/$base.o"
done
"$CC" -B "$ROOT" -linux -o build/lua $objs

cat > build/lua-smoke.lua << 'EOF'
assert(1 + 1 == 2)
assert(2 ^ 10 == 1024)
assert(2 ^ -3 == 0.125)
assert(math.sqrt(4) == 2)
assert(math.floor(-1.5) == -2)
assert(math.fmod(5.5, 2) == 1.5)
assert(string.sub("abc", 2, 2) == "b")
local ok = pcall(function() error("x") end)
assert(ok == false)
assert(tonumber("1.5") == 1.5)
assert(tonumber("0x10") == 16)
assert(os.date("%Y-%m-%d", 0) == "1970-01-01")
print("lua smoke ok")
EOF
lua_bin=$PWD/build/lua
./build/lua build/lua-smoke.lua

# Ядро языка. files.lua и main.lua требуют popen и разбор командной строки.
cd "$src/testes"
fail=0
for t in math strings nextvar events calls literals sort goto locals constructs \
         code vararg closure coroutine errors bitwise utf8 pm; do
    if [ ! -f "$t.lua" ]; then
        echo "skip $t (no testes/)"
        continue
    fi
    echo "== $t"
    if ! "$lua_bin" "$t.lua"; then
        echo "FAIL $t"
        fail=1
    fi
done
exit $fail
