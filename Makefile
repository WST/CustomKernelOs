# MyOS — сборка
#
#   make           — собрать ядро (build/os.img), программы и установить их в /bin на disk.img
#   make run       — всё собрать и запустить в QEMU
#   make test      — юнит-тесты файловой системы и кучи (на хосте)
#   make jstest    — тесты JavaScript, DOM и CSS браузера (на хосте)
#   make gfxtest   — пробная картинка сглаженной графики и шрифтов (build/gfxtest.ppm)
#   make clean     — удалить build/ (диск с файлами disk.img НЕ трогается)
#   make cleandisk — удалить disk.img (при следующем запуске ФС создастся заново)
#
#   build/myfs disk.img ls /bin            — посмотреть диск с компьютера
#   build/myfs disk.img put file.txt /f    — положить свой файл на диск
#   build/myfs disk.img cat /etc/display   — вывести файл с диска

# На macOS (и вообще не на x86-Linux) системный gcc — это clang под arm64/x86_64-darwin,
# он не умеет собирать 32-битный ELF. Нужен кросс-тулчейн:
#   brew install i686-elf-gcc i686-elf-binutils nasm qemu
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CROSS   ?= i686-elf-
endif
CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AR      := $(CROSS)ar
NASM    := nasm
OBJCOPY := $(CROSS)objcopy
QEMU    := qemu-system-i386
HOSTCC  ?= cc

# переносимые (GNU/BSD) размер файла и дополнение файла нулями до размера
fsize   = $$(wc -c < $(1) | tr -d ' ')
padto   = dd if=/dev/null of=$(1) bs=1 seek=$(2) 2>/dev/null

CFLAGS  := -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
           -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx -mno-80387 \
           -O2 -Wall -Wextra -std=gnu11
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib \
           $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx && echo --no-warn-rwx-segments)

BUILD   := build
DISK    := disk.img
DISK_MB := 4
C_SRC   := $(wildcard kernel/*.c)
OBJS    := $(BUILD)/entry.o $(BUILD)/isr.o $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRC))

all: $(BUILD)/os.img install

$(BUILD) $(BUILD)/user:
	mkdir -p $@

# ------------------------------------------------------------------ ядро

$(BUILD)/%.o: kernel/%.c kernel/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# Загрузчику нужно знать размер ядра в секторах — считаем и передаём в NASM
$(BUILD)/boot.bin: boot/boot.asm $(BUILD)/kernel.bin
	$(NASM) -f bin -DKERNEL_SECTORS=$$(( ($(call fsize,$(BUILD)/kernel.bin) + 511) / 512 )) $< -o $@

# Образ: [сектор 0: boot.bin][сектора 1..N: kernel.bin], дополнен до 1.44 МБ
$(BUILD)/os.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin
	cat $^ > $@
	$(call padto,$@,1474560)
	@echo "==> $@ готов (ядро: $(call fsize,$(BUILD)/kernel.bin) байт)"

# ------------------------------------------------------------------ программы (ring 3)

# программам FPU можно (ядро сохраняет его регистры при переключении), SSE — нет
UCFLAGS  := -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
            -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx \
            -O2 -Wall -std=gnu11 -Iuser/include -Iuser/lib -ffunction-sections -fdata-sections
# --gc-sections и библиотека-архив: в программу попадает только то, что она зовёт
ULDFLAGS := -m elf_i386 -T user/user.ld -nostdlib -s --gc-sections
PROGRAMS := $(basename $(notdir $(wildcard user/*.c))) cc
UBINS    := $(addprefix $(BUILD)/user/,$(PROGRAMS))

ULIB_OBJS := $(BUILD)/user/libc.o $(BUILD)/user/gfx.o $(BUILD)/user/vec.o $(BUILD)/user/font.o $(BUILD)/user/win.o
ULIB := $(BUILD)/user/libmyos.a

$(ULIB): $(ULIB_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/user/libc.o: user/lib/libc.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/gfx.o: user/lib/gfx.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/vec.o: user/lib/vec.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/font.o: user/lib/font.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/win.o: user/lib/win.c user/lib/win.h user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.c user/lib/myos.h user/lib/gfx.h user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%: $(BUILD)/user/%.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

# HTTPS (TLS 1.3) — user/lib/tls.c, только тем, кому нужен: browser и wget.
# Собирается gcc (нужна 64-битная арифметика, которой нет в cc внутри MyOS).
TLS_PROGS := $(BUILD)/user/wget

# x509.c — проверка сертификатов (RSA, ECDSA P-256/P-384, цепочка до корней из /etc/certs.der)
TLS_OBJS := $(BUILD)/user/tls.o $(BUILD)/user/x509.o

$(BUILD)/user/tls.o: user/lib/tls.c user/lib/tls.h user/lib/x509.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/x509.o: user/lib/x509.c user/lib/x509.h user/lib/tls.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(patsubst %,%.o,$(TLS_PROGS)) $(BUILD)/user/browser.o: user/lib/tls.h

$(TLS_PROGS): $(BUILD)/user/%: $(BUILD)/user/%.o $(ULIB) $(TLS_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) $< $(TLS_OBJS) $(ULIB) -o $@

# JavaScript — движок user/lib/js*.c (программа js и браузер) и DOM user/lib/dom*.c (браузер).
# Тоже собирается gcc: в нём 64-битная арифметика и сопроцессор x87.
JSLIB    := js jsparse jsrun jsnum jslib jslib2 jsre
JS_OBJS  := $(patsubst %,$(BUILD)/user/l_%.o,$(JSLIB))
DOM_OBJS := $(BUILD)/user/l_dom.o $(BUILD)/user/l_domjs.o $(BUILD)/user/l_css.o

$(BUILD)/user/l_%.o: user/lib/%.c user/lib/js.h user/lib/jsint.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(DOM_OBJS) $(BUILD)/user/browser.o: user/lib/dom.h user/lib/domint.h user/lib/js.h
$(BUILD)/user/l_domjs.o: user/lib/dom_prelude.h

# прелюдия (часть окружения браузера на JS) -> строка Си
user/lib/dom_prelude.h: user/lib/dom_prelude.js
	sed -e '/^ *\/\//d' -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

$(BUILD)/user/js.o: user/lib/js.h

$(BUILD)/user/js: $(BUILD)/user/js.o $(ULIB) $(JS_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) $< $(JS_OBJS) $(ULIB) -o $@

# картинки (PNG, GIF, JPEG, BMP) — user/lib/img.c, пока только браузеру
$(BUILD)/user/l_img.o: user/lib/img.c user/lib/img.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/browser.o: user/lib/img.h

$(BUILD)/user/browser: $(BUILD)/user/browser.o $(ULIB) $(TLS_OBJS) $(BUILD)/user/l_img.o $(JS_OBJS) $(DOM_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) $< $(TLS_OBJS) $(BUILD)/user/l_img.o $(JS_OBJS) $(DOM_OBJS) $(ULIB) -o $@

# cc — компилятор Си, который работает внутри MyOS (несколько файлов в user/cc/)
CC_SRC  := $(wildcard user/cc/*.c)
CC_OBJS := $(patsubst user/cc/%.c,$(BUILD)/user/cc_%.o,$(CC_SRC))

$(BUILD)/user/cc_%.o: user/cc/%.c user/cc/cc.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/cc: $(CC_OBJS) $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $(CC_OBJS) $(ULIB) -o $@

# cc для Linux — чтобы быстро гонять тесты компилятора (make cctest)
$(BUILD)/cc-host: $(CC_SRC) user/cc/cc.h | $(BUILD)
	$(HOSTCC) -std=gnu17 -DCC_HOST -O1 -g -Wall -Wno-unused-function -o $@ $(CC_SRC)

programs: $(UBINS)

# ------------------------------------------------------------------ утилита myfs (хост)

$(BUILD)/myfs: tools/myfs.c tools/host_io.c kernel/fs.c kernel/myfs.c kernel/myfs.h kernel/block.c kernel/block.h kernel/fs.h kernel/string.c | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/host_io.o
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tools/myfs.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c \
	    $(BUILD)/host_io.o -o $@

# Диск: если его нет — создаём пустой (myfs сам его отформатирует)
$(DISK):
	$(call padto,$@,$$(( $(DISK_MB) * 1048576 )))

# Установить программы в /bin (остальные файлы на диске не трогаются).
# Сторонние утилиты — в /ext/bin под своими именами, чтобы не затереть /bin.
install: $(UBINS) $(BUILD)/user/owc $(BUILD)/user/ohead $(BUILD)/user/ols $(BUILD)/myfs | $(DISK)
	@echo "==> устанавливаю программы в /bin на $(DISK)"
	@for p in $(PROGRAMS); do $(BUILD)/myfs $(DISK) put $(BUILD)/user/$$p /bin/$$p || exit 1; done
	@echo "==> сторонние программы в /ext/bin"
	@$(BUILD)/myfs $(DISK) mkdir /ext/bin >/dev/null
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/owc /ext/bin/wc || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/ohead /ext/bin/head || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/ols /ext/bin/ls || exit 1
	@echo "==> заголовки в /include, библиотеки в /lib, исходники программ в /src (для cc)"
	@for d in /include /lib /lib/fonts /src /src/cc /etc /home /root /tmp /var /var/lib /var/log /usr /usr/bin /usr/lib /usr/share /mnt /media /proc /sys /run /ext /ext/bin; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null 2>&1 || true; done
	@for f in myos.h gfx.h win.h tls.h; do $(BUILD)/myfs $(DISK) put user/lib/$$f /include/$$f >/dev/null || exit 1; done
	@for f in user/include/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) mkdir /include/sys >/dev/null 2>&1 || true
	@for f in user/include/sys/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/sys/$$(basename $$f) >/dev/null || exit 1; done
	@for f in libc.c gfx.c vec.c font.c win.c tls.c; do $(BUILD)/myfs $(DISK) put user/lib/$$f /lib/$$f >/dev/null || exit 1; done
	@for f in user/*.c; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@for f in user/cc/*.c user/cc/cc.h; do $(BUILD)/myfs $(DISK) put $$f /src/cc/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> шрифты Ubuntu в /lib/fonts"
	@for f in fonts/*.ttf; do $(BUILD)/myfs $(DISK) put $$f /lib/fonts/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> корневые сертификаты (список Mozilla) в /etc/certs.der"
	@$(BUILD)/myfs $(DISK) mkdir /etc >/dev/null 2>&1 || true
	@$(BUILD)/myfs $(DISK) put certs/roots.der /etc/certs.der >/dev/null

# ------------------------------------------------------------------ запуск

# index=0 — primary master (загрузка), index=1 — primary slave (MyFS)
QEMU_DRIVES := -drive format=raw,file=$(BUILD)/os.img,index=0,media=disk \
               -drive format=raw,file=$(DISK),index=1,media=disk
QEMU_MEM ?= 256M
# Optional independent data disk on secondary master: make run DATA_DISK=data.img
DATA_DISK ?=
ifneq ($(strip $(DATA_DISK)),)
QEMU_DRIVES += -drive format=raw,file=$(DATA_DISK),index=2,media=disk
endif
# видеокарта: vmware — VMware SVGA II с ускорением 2D и аппаратным курсором (по умолчанию),
# std — простая Bochs VBE: make run VGA=std
VGA ?= vmware
# сеть: карта e1000 за NAT QEMU; HOST_PORT хоста -> порт 80 MyOS (httpd)
# Другой порт: make run HOST_PORT=8081
HOST_PORT ?= 8080
QEMU_NET := -nic user,model=e1000,hostfwd=tcp::$(HOST_PORT)-:80 -rtc base=localtime

# всё, что пишет консоль ядра (и browser -v), — ещё и в build/serial.log
run: all
	$(QEMU) -m $(QEMU_MEM) -vga $(VGA) $(QEMU_DRIVES) $(QEMU_NET) -serial file:$(BUILD)/serial.log

debug: all
	$(QEMU) -vga $(VGA) $(QEMU_DRIVES) $(QEMU_NET) -no-reboot -d int,cpu_reset -s -S

clean:
	rm -rf $(BUILD)

cleandisk:
	rm -f $(DISK)

# Юнит-тесты (запускаются на хосте, не в QEMU)
test: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -Wall -Wno-unused-function -Iuser/lib tests/tls_test.c -o $(BUILD)/tls_test
	./$(BUILD)/tls_test
	$(HOSTCC) -std=gnu17 -O1 -w -Iuser/lib tests/x509_test.c -o $(BUILD)/x509-test
	./$(BUILD)/x509-test
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/fs_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c -o $(BUILD)/fs_test
	./$(BUILD)/fs_test
	gcc -std=gnu17 -O1 -w -fno-builtin -Ikernel -include tests/host_env.h \
	    tests/heap_test.c kernel/heap.c kernel/string.c -o $(BUILD)/heap_test
	./$(BUILD)/heap_test

# Тесты JavaScript на Linux: язык и встроенные объекты, потом DOM браузера на тестовой странице
JS_SRC := $(patsubst %,user/lib/%.c,$(JSLIB))
jstest: user/lib/dom_prelude.h | $(BUILD)
	gcc -std=gnu17 -DJS_HOST -O1 -g -Wall -Wno-unused-parameter -Wno-unused-function -Iuser/lib tests/js_host.c $(JS_SRC) -o $(BUILD)/js-host
	gcc -std=gnu17 -DJS_HOST -O1 -g -Wall -Wno-unused-parameter -Wno-unused-function -Iuser/lib tests/dom_host.c $(JS_SRC) \
	    user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $(BUILD)/dom-host
	./$(BUILD)/js-host tests/js/lang.js | tee $(BUILD)/jstest.out
	@grep -q " 0 failed" $(BUILD)/jstest.out
	./$(BUILD)/js-host tests/js/gen.js > $(BUILD)/gentest.out
	@diff -u tests/js/gen.expected $(BUILD)/gentest.out && echo "generators/async: OK"
	./$(BUILD)/dom-host tests/web/t1.html click:btn click:lnk key:q:ab wait:200 > $(BUILD)/domtest.out
	@diff -u tests/web/t1.expected $(BUILD)/domtest.out && echo "DOM test: OK"
	./$(BUILD)/dom-host tests/web/async.html wait:300 > $(BUILD)/asynctest.out
	@diff -u tests/web/async.expected $(BUILD)/asynctest.out && echo "DOM async test: OK"
	./$(BUILD)/dom-host tests/web/css.html styles > $(BUILD)/csstest.out
	@diff -u tests/web/css.expected $(BUILD)/csstest.out && echo "CSS test: OK"

# Картинки и сертификаты на образцах (нужен python3 с Pillow и cryptography: pip install pillow cryptography)
imgtest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -w -Iuser/lib -DIMG_HOST tests/img_test.c user/lib/img.c -o $(BUILD)/img-test
	python3 tests/img/make.py $(BUILD)/imgt
	@fails=0; while read n w h t; do ./$(BUILD)/img-test $(BUILD)/imgt/$$n $(BUILD)/imgt/$$n.rgba $$w $$h $$t || fails=1; done < $(BUILD)/imgt/cases.txt; \
	    [ $$fails = 0 ] && echo "images: OK"

x509test: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -w -Iuser/lib tests/x509_test.c -o $(BUILD)/x509-test
	python3 tests/x509/make.py $(BUILD)/x509t
	./$(BUILD)/x509-test $(BUILD)/x509t

# Весь браузер (загрузка, DOM, JS, CSS, раскладка, картинки) на Linux с AddressSanitizer:
#   build/browser-host страница.html [w 1280] [scroll] [click] [keys] [images] [timers 500] [shot f.ppm]
browsertest: user/lib/dom_prelude.h | $(BUILD)
	gcc -m32 -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-builtin -nostdinc -w -Iuser/lib \
	    tests/browser_host.c user/lib/win.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/tls.c user/lib/x509.c \
	    user/lib/img.c $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $(BUILD)/browser-host
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/t1.html scroll w 1280 scroll w 480 click keys timers 200
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/css.html scroll w 1000 images

# Сглаженная графика и шрифты на Linux: пробная картинка build/gfxtest.ppm
gfxtest: | $(BUILD)
	gcc -std=gnu17 -m32 -O1 -g -fno-builtin -nostdinc -w -Iuser/lib tests/gfx_host.c user/lib/gfx.c user/lib/vec.c \
	    user/lib/font.c -o $(BUILD)/gfx-host
	./$(BUILD)/gfx-host fonts > $(BUILD)/gfxtest.ppm && echo "==> $(BUILD)/gfxtest.ppm"

# Тесты компилятора cc на Linux: тестовые программы и «cc собирает сам себя»
CCROOT := $(BUILD)/ccroot
cctest: $(BUILD)/cc-host
	@mkdir -p $(CCROOT)/include $(CCROOT)/lib
	@cp user/lib/myos.h user/lib/gfx.h user/lib/win.h user/include/*.h $(CCROOT)/include/
	@mkdir -p $(CCROOT)/include/sys
	@cp user/include/sys/*.h $(CCROOT)/include/sys/
	@cp user/lib/libc.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c $(CCROOT)/lib/
	@echo 'int main(void) { return 0; }' > $(BUILD)/cct_probe.c
	@$(BUILD)/cc-host -B $(CCROOT) -linux $(BUILD)/cct_probe.c -o $(BUILD)/cct_probe
	@if ! ./$(BUILD)/cct_probe 2>/dev/null; then \
	    echo "32-bit programs cannot run here - only checking that tests and cc itself compile"; \
	    for t in tests/cc/*.c; do $(BUILD)/cc-host -B $(CCROOT) -linux $$t -o $(BUILD)/cct_x || exit 1; done; \
	    $(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && echo "compile-only: OK"; \
	    exit 0; \
	fi; \
	for t in tests/cc/*.c; do \
	    n=$$(basename $$t .c); \
	    $(BUILD)/cc-host -B $(CCROOT) -linux $$t -o $(BUILD)/cct_$$n && ./$(BUILD)/cct_$$n || exit 1; \
	done; \
	rm -f $(BUILD)/cct_cc1 $(BUILD)/cct_cc2 $(BUILD)/cct_cc3; \
	$(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && \
	./$(BUILD)/cct_cc1 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc2 && \
	./$(BUILD)/cct_cc2 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc3 && \
	cmp $(BUILD)/cct_cc2 $(BUILD)/cct_cc3 && echo "cc compiles itself: OK (stage 2 == stage 3)"

# OpenBSD wc: cc -linux запускает утилиту на хосте, gcc -m32 собирает ELF для MyOS
$(BUILD)/user/owc: third_party/openbsd/wc.c $(ULIB) user/user.ld | $(BUILD)/user
	$(CC) $(UCFLAGS) -c third_party/openbsd/wc.c -o $(BUILD)/user/owc.o
	$(LD) $(ULDFLAGS) -o $@ $(BUILD)/user/owc.o $(ULIB)

$(BUILD)/user/ohead: third_party/openbsd/head.c $(ULIB) user/user.ld | $(BUILD)/user
	$(CC) $(UCFLAGS) -c third_party/openbsd/head.c -o $(BUILD)/user/ohead.o
	$(LD) $(ULDFLAGS) -o $@ $(BUILD)/user/ohead.o $(ULIB)

$(BUILD)/user/ols: third_party/ls/ls.c $(ULIB) user/user.ld | $(BUILD)/user
	$(CC) $(UCFLAGS) -c third_party/ls/ls.c -o $(BUILD)/user/ols.o
	$(LD) $(ULDFLAGS) -o $@ $(BUILD)/user/ols.o $(ULIB)

porttest: $(BUILD)/cc-host $(BUILD)/user/owc $(BUILD)/user/ohead $(BUILD)/user/ols
	@mkdir -p $(CCROOT)/include/sys $(CCROOT)/lib
	@cp user/lib/myos.h user/lib/gfx.h user/lib/win.h user/include/*.h $(CCROOT)/include/
	@cp user/include/sys/*.h $(CCROOT)/include/sys/
	@cp user/lib/libc.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c $(CCROOT)/lib/
	@printf 'hello world\n' > $(BUILD)/wc-in
	@$(BUILD)/cc-host -B $(CCROOT) -linux third_party/openbsd/wc.c -o $(BUILD)/wc-linux
	@$(BUILD)/wc-linux $(BUILD)/wc-in > $(BUILD)/wc-out
	@printf '       1       2      12 %s\n' $(BUILD)/wc-in > $(BUILD)/wc-exp
	@cmp $(BUILD)/wc-out $(BUILD)/wc-exp && echo "wc: lines/words/chars OK"
	@$(BUILD)/wc-linux -c $(BUILD)/wc-in > $(BUILD)/wc-c
	@printf '      12 %s\n' $(BUILD)/wc-in > $(BUILD)/wc-cexp
	@cmp $(BUILD)/wc-c $(BUILD)/wc-cexp && echo "wc -c: OK"
	@$(BUILD)/cc-host -B $(CCROOT) -linux third_party/openbsd/head.c -o $(BUILD)/head-linux
	@printf 'a\nb\nc\nd\n' > $(BUILD)/head-in
	@$(BUILD)/head-linux -n 2 $(BUILD)/head-in > $(BUILD)/head-out
	@printf 'a\nb\n' > $(BUILD)/head-exp
	@cmp $(BUILD)/head-out $(BUILD)/head-exp && echo "head -n: OK"
	@$(BUILD)/head-linux -2 $(BUILD)/head-in > $(BUILD)/head-out2
	@cmp $(BUILD)/head-out2 $(BUILD)/head-exp && echo "head -count: OK"
	@rm -rf $(BUILD)/ls-dir && mkdir $(BUILD)/ls-dir
	@printf 'x\n' > $(BUILD)/ls-dir/b && printf 'y\n' > $(BUILD)/ls-dir/a
	@$(BUILD)/cc-host -B $(CCROOT) -linux third_party/ls/ls.c -o $(BUILD)/ls-linux
	@$(BUILD)/ls-linux $(BUILD)/ls-dir | sort > $(BUILD)/ls-out
	@printf 'a\nb\n' > $(BUILD)/ls-exp
	@cmp $(BUILD)/ls-out $(BUILD)/ls-exp && echo "ls: names OK"
	@$(BUILD)/ls-linux $(BUILD)/ls-dir/a > $(BUILD)/ls-file
	@printf '%s\n' $(BUILD)/ls-dir/a > $(BUILD)/ls-file-exp
	@cmp $(BUILD)/ls-file $(BUILD)/ls-file-exp && echo "ls: file OK"

vfstest: all
	python3 tests/vfs_smoke.py

largetest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/large-host-io.o
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/large_fs_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/large-host-io.o -o $(BUILD)/large-fs-test
	python3 tests/run_large_fs.py $(BUILD)/large-fs-test

.PHONY: all run debug clean cleandisk test programs install cctest porttest jstest gfxtest vfstest largetest
.SECONDARY:


