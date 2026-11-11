#!/bin/sh
# make на хосте через cc -linux. Аргумент — собранный бинарник.
set -u
G=${1:?need make binary}
case "$G" in
    /*) ;;
    *) G=$(pwd)/$G ;;
esac
D=$(mktemp -d)
fail() { echo "FAIL: $1"; rm -rf "$D"; exit 1; }
cd "$D" || exit 1

# Явные правила: первая сборка, повторный запуск, пересборка после удаления результата.
cat > Makefile << 'EOF'
all: prog

prog: a.o
	cat a.o > prog

a.o: a.c
	cat a.c > a.o
EOF
printf 'hello\n' > a.c
"$G" > run1.out || fail first
cmp run1.out - << 'EOF' || fail first-out
cat a.c > a.o
cat a.o > prog
EOF
printf 'hello\n' > expect.txt
cmp prog expect.txt || fail prog-bytes
"$G" > run2.out || fail second
printf "make: 'all' is up to date\n" > up.txt
cmp run2.out up.txt || fail second-out
rm -f a.o
"$G" > run3.out || fail rebuild
cmp run3.out run1.out || fail rebuild-out
cmp prog expect.txt || fail rebuild-bytes

# Шаблон, переменные = и :=, тихий рецепт.
rm -rf "$D"/*
cd "$D" || exit 1
cat > Makefile << 'EOF'
B = z
C := $(B)
B = y
A = $(B)

all: foo.out vars

%.out: %.in
	cat $< > $@

vars:
	@echo $(C) $(A) > vars
EOF
printf 'pattern\n' > foo.in
"$G" > pat.out || fail pattern
grep -q 'cat foo.in > foo.out' pat.out || fail pattern-cmd
grep -q 'echo' pat.out && fail pattern-quiet || true
printf 'pattern\n' > expect-pat.txt
cmp foo.out expect-pat.txt || fail pattern-bytes
printf 'z y\n' > expect-vars.txt
cmp vars expect-vars.txt || fail vars

# .PHONY заставляет пересобрать и тех, кто от неё зависит.
rm -rf "$D"/*
cd "$D" || exit 1
cat > Makefile << 'EOF'
.PHONY: stamp
all: keeper

keeper: stamp
	cat stamp > keeper

stamp:
	echo ran >> log
	echo x > stamp
EOF
"$G" > /dev/null || fail phony1
"$G" > /dev/null || fail phony2
printf 'ran\nran\n' > expect-log.txt
cmp log expect-log.txt || fail phony-log

# Нет правила, цикл, код возврата рецепта, -n не создаёт файл.
rm -rf "$D"/*
cd "$D" || exit 1
printf 'all: missing.c\n' > Makefile
"$G" > miss.out 2> miss.err && fail missing-ok || true
grep -q "no rule to make target 'missing.c'" miss.err || fail missing-msg

printf 'all: a\na: b\nb: a\n' > Makefile
"$G" > cyc.out 2> cyc.err && fail cycle-ok || true
grep -q 'circular dependency' cyc.err || fail cycle-msg

printf 'all:\n\texit 1\n' > Makefile
"$G" > bad.out 2> bad.err && fail recipe-ok || true
grep -q 'Error 1' bad.err || fail recipe-msg

printf 'all: t\nt:\n\techo hi > t\n' > Makefile
"$G" -n > dry.out || fail dry
grep -q 'echo hi > t' dry.out || fail dry-out
test ! -f t || fail dry-created

# -C и -f, нижний регистр makefile.
rm -rf "$D"/*
cd "$D" || exit 1
mkdir sub
printf 'all:\n\techo side > marker\n' > sub/other.mk
"$G" -C sub -f other.mk > /dev/null || fail dash-cf
printf 'side\n' > expect-side.txt
cmp sub/marker expect-side.txt || fail dash-cf-bytes
printf 'all:\n\techo lower > lower\n' > makefile
rm -f Makefile
"$G" > /dev/null || fail lower
printf 'lower\n' > expect-lower.txt
cmp lower expect-lower.txt || fail lower-bytes

rm -rf "$D"
echo "make: ok"
