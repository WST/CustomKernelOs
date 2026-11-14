# Сборка SlimCC нашим cc. Чужой Makefile использует присваивание !=, которого у make нет.
# Платформа — та, что в дереве называется «просто работает». VLA в bitint отключены.
CC = cc
CFLAGS = -DBOOTSTRAP_NO_VLA

OBJS = alloc.o bitint.o codegen.o hashmap.o main.o parse.o platform.o preprocess.o strings.o tokenize.o type.o unicode.o

all: slimcc

slimcc: platform.c $(OBJS)
	$(CC) $(CFLAGS) -o slimcc $(OBJS)

platform.c: platform/linux-glibc-generic.c
	cp platform/linux-glibc-generic.c platform.c

%.o: %.c slimcc.h
	$(CC) $(CFLAGS) -c $< -o $@

install: slimcc
	mkdir -p /ext/bin
	cp slimcc /ext/bin/slimcc
	cp -r slimcc_headers /ext/bin/slimcc_headers
