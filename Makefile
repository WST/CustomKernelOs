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
#   build/myfs disk.img grow 128           — расширить ФС до 128 МБ, старый образ остаётся как disk.img.bak

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
# Новый disk.img, если файла ещё нет. Готовый образ сам не растёт:
# build/myfs disk.img grow 64  (прежний остаётся как disk.img.bak)
DISK_MB := 64
C_SRC   := $(wildcard kernel/*.c)
OBJS    := $(BUILD)/entry.o $(BUILD)/isr.o $(BUILD)/ap.o $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRC))

all: $(BUILD)/os.img install

$(BUILD)/user/launchbench.o: tests/launchbench.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/launchbench: $(BUILD)/user/launchbench.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/spawncheck.o: tests/spawncheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/spawncheck: $(BUILD)/user/spawncheck.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/desktop_launchcheck.o: tests/desktop_launchcheck.c user/desktop.c user/desktop_gpu.inc user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/desktop_launchcheck: $(BUILD)/user/desktop_launchcheck.o $(BUILD)/user/l_img.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

.PHONY: launchtest
launchtest:
	python3 tests/launch_smoke.py

.PHONY: smptest
smptest:
	python3 tests/smp_smoke.py

$(BUILD)/user/smpcheck.o: tests/smpcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/smpcheck: $(BUILD)/user/smpcheck.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/smpbench.o: tests/smpbench.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/smpbench: $(BUILD)/user/smpbench.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: FORCE_BUILD_INFO
FORCE_BUILD_INFO:
$(BUILD)/build_info.h: FORCE_BUILD_INFO VERSION tools/build_info.py | $(BUILD)
	python3 tools/build_info.py $@
$(BUILD)/virtgpu.o: kernel/virtgpu_render.inc kernel/virtgpu_cursor.inc
$(BUILD)/sysinfo.o: $(BUILD)/build_info.h
$(OBJS): user/lib/sysinfo.h
$(BUILD)/user/libc.o: user/lib/sysinfo.h
CFLAGS += -I$(BUILD)


# Native clipboard/editor checks, installed only onto a disposable test disk.
$(BUILD)/user/clipboardcheck.o: tests/clipboardcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/clipboard_editor.o: tests/clipboard_editor.c user/notepad.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/clipboard_terminal.o: tests/clipboard_terminal.c user/terminal.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/clipboardcheck: $(BUILD)/user/clipboardcheck.o $(BUILD)/user/clipboard_editor.o $(BUILD)/user/clipboard_terminal.o $(BUILD)/user/libmyos.a
	$(LD) -m elf_i386 -T user/user.ld -nostdlib -s --gc-sections $^ -o $@

$(BUILD)/user/pr2check.o: tests/pr2check.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/pr2check: $(BUILD)/user/pr2check.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: pr2test
pr2test:
	python3 tests/pr2_smoke.py

.PHONY: clipboardtest
clipboardtest:
	python3 tests/clipboard_smoke.py
	python3 tests/clipboard_gui_smoke.py

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
# ldso.c — не команда /bin, а исходник /lib/ld.so (собирается отдельно, своим cc).
PROGRAMS := $(filter-out ldso,$(basename $(notdir $(wildcard user/*.c)))) cc
UBINS    := $(addprefix $(BUILD)/user/,$(PROGRAMS))

ULIB_OBJS := $(BUILD)/user/libc.o $(BUILD)/user/gfx.o $(BUILD)/user/vec.o $(BUILD)/user/font.o $(BUILD)/user/win.o
ULIB := $(BUILD)/user/libmyos.a

$(ULIB): $(ULIB_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/user/libc.o: user/lib/libc.c user/lib/regex.c user/include/regex.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/gfx.o: user/lib/gfx.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/vec.o: user/lib/vec.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/font.o: user/lib/font.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/win.o: user/lib/win.c user/lib/win.h user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/code.o: user/code_features.h user/code_index.h user/code_workspace.h user/code_chrome.h user/code_signature.h

$(BUILD)/user/%.o: user/%.c user/lib/myos.h user/lib/sysinfo.h user/lib/gfx.h user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

# sh и cc остаются статическими: оболочка и компилятор стартуют даже со сломанной libc.so.
$(BUILD)/user/sh: $(BUILD)/user/sh.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

# Динамические программы: libc.so/libgfx.so, интерпретатор /lib/ld.so.
DYNLDFLAGS := -m elf_i386 -nostdlib -s --gc-sections -dynamic-linker /lib/ld.so \
              --hash-style=sysv --as-needed -Ttext-segment=0x40000000

$(BUILD)/user/crt0.o: user/lib/crt0.asm | $(BUILD)/user
	$(NASM) -f elf32 $< -o $@

$(BUILD)/user/libc.so: user/lib/libc.c user/lib/regex.c user/include/regex.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -fPIC -D__LIBC_SO__ -shared -Wl,-soname,libc.so -Wl,--hash-style=sysv -o $@ $<

$(BUILD)/user/pic/%.o: user/lib/%.c user/lib/myos.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC -c $< -o $@

$(BUILD)/user/pic: | $(BUILD)/user
	mkdir -p $@

PIC_GFX := $(patsubst %,$(BUILD)/user/pic/%.o,gfx vec font win)
$(BUILD)/user/pic/gfx.o: user/lib/gfx.h
$(BUILD)/user/pic/vec.o: user/lib/gfx.h
$(BUILD)/user/pic/font.o: user/lib/gfx.h
$(BUILD)/user/pic/win.o: user/lib/win.h user/lib/gfx.h

$(BUILD)/user/libgfx.so: $(PIC_GFX) $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libgfx.so -Wl,--hash-style=sysv -o $@ $(PIC_GFX) -L$(BUILD)/user -lc

$(BUILD)/user/%: $(BUILD)/user/%.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< -L$(BUILD)/user -lc -lgfx

# HTTPS (TLS 1.3) — user/lib/tls.c, только тем, кому нужен: browser и wget.
# Собирается gcc (нужна 64-битная арифметика, которой нет в cc внутри MyOS).
TLS_PROGS := $(BUILD)/user/wget $(BUILD)/user/download

# x509.c — проверка сертификатов (RSA, ECDSA P-256/P-384, цепочка до корней из /etc/certs.der)
TLS_OBJS := $(BUILD)/user/tls.o $(BUILD)/user/x509.o

$(BUILD)/user/tls.o: user/lib/tls.c user/lib/tls.h user/lib/x509.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/x509.o: user/lib/x509.c user/lib/x509.h user/lib/tls.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(patsubst %,%.o,$(TLS_PROGS)) $(BUILD)/user/browser.o: user/lib/tls.h

$(TLS_PROGS): $(BUILD)/user/%: $(BUILD)/user/%.o $(TLS_OBJS) $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< $(TLS_OBJS) -L$(BUILD)/user -lc -lgfx

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

$(BUILD)/user/js: $(BUILD)/user/js.o $(JS_OBJS) $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< $(JS_OBJS) -L$(BUILD)/user -lc -lgfx

# картинки (PNG, GIF, JPEG, BMP) — user/lib/img.c, пока только браузеру
$(BUILD)/user/l_img.o: user/lib/img.c user/lib/img.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/l_svg.o: user/lib/svg.c user/lib/svg.h user/lib/gfx.h user/lib/js.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/browser.o: user/lib/img.h user/lib/svg.h

$(BUILD)/user/browser: $(BUILD)/user/browser.o $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) -L$(BUILD)/user -lc -lgfx

# Native browser paint/scroll benchmark (not installed by default).
$(BUILD)/user/browser-paint.o: tests/browser_paint.c user/browser.c user/lib/dom.h user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/browser-paint: $(BUILD)/user/browser-paint.o $(ULIB) $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) $< $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) $(ULIB) -o $@

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

# Библиотеки для cc — статические архивы, собранные самим cc: /lib/libc.a и
# /lib/libgfx.a на диске MyOS ($(BUILD)/mlib) и такие же для cc-host -linux ($(CCROOT)/lib).
CCROOT    := $(BUILD)/ccroot
CC_HDRS   := user/lib/myos.h user/lib/sysinfo.h user/lib/gfx.h user/lib/win.h $(wildcard user/include/*.h) $(wildcard user/include/sys/*.h)
CC_LIBSRC := user/lib/libc.c user/lib/regex.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c
GFX_PARTS := gfx vec font win

$(CCROOT)/.stamp: $(CC_HDRS) $(CC_LIBSRC)
	@mkdir -p $(CCROOT)/include/sys $(CCROOT)/lib
	@cp user/lib/myos.h user/lib/sysinfo.h user/lib/gfx.h user/lib/win.h user/include/*.h $(CCROOT)/include/
	@cp user/include/sys/*.h $(CCROOT)/include/sys/
	@cp $(CC_LIBSRC) $(CCROOT)/lib/
	@touch $@

$(BUILD)/libobj-linux/%.o: user/lib/%.c $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux -c $< -o $@

$(BUILD)/libobj-myos/%.o: user/lib/%.c $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -c $< -o $@

$(BUILD)/libobj-linux/libc.o $(BUILD)/libobj-myos/libc.o: user/lib/regex.c

$(CCROOT)/lib/libc.a: $(BUILD)/libobj-linux/libc.o
	rm -f $@ && $(AR) rcs $@ $^
$(CCROOT)/lib/libgfx.a: $(patsubst %,$(BUILD)/libobj-linux/%.o,$(GFX_PARTS))
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libc.a: $(BUILD)/libobj-myos/libc.o
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libgfx.a: $(patsubst %,$(BUILD)/libobj-myos/%.o,$(GFX_PARTS))
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^

CCROOT_READY := $(CCROOT)/.stamp $(CCROOT)/lib/libc.a $(CCROOT)/lib/libgfx.a
MLIBS        := $(BUILD)/mlib/libc.a $(BUILD)/mlib/libgfx.a

# ------------------------------------------------------------------ утилита myfs (хост)

$(BUILD)/myfs: tools/myfs.c tools/host_io.c kernel/fs.c kernel/myfs.c kernel/myfs.h kernel/block.c kernel/block.h kernel/fs.h kernel/string.c | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/host_io.o
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tools/myfs.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c \
	    $(BUILD)/host_io.o -o $@

# Диск: если его нет — создаём пустой (myfs сам его отформатирует)
$(DISK):
	$(call padto,$@,$$(( $(DISK_MB) * 1048576 )))

# Установить программы в /bin (остальные файлы на диске не трогаются).
# Сторонние утилиты ставятся в /ext/bin командой ext, по файлам /ext/instructions.
$(BUILD)/ld.so: user/ldso.c $(BUILD)/cc-host $(CCROOT_READY)
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $@ user/ldso.c

install: $(UBINS) $(MLIBS) $(BUILD)/myfs $(BUILD)/ld.so | $(DISK)
	@echo "==> устанавливаю программы в /bin на $(DISK)"
	@for p in $(PROGRAMS); do $(BUILD)/myfs $(DISK) put $(BUILD)/user/$$p /bin/$$p || exit 1; done
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/gzip /bin/gunzip || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/ar /bin/ranlib || exit 1
	@echo "==> инструкции сторонних программ в /ext/instructions"
	@$(BUILD)/myfs $(DISK) mkdir /ext/bin >/dev/null
	@$(BUILD)/myfs $(DISK) mkdir /ext/instructions >/dev/null
	@$(BUILD)/myfs $(DISK) rm /ext/bin/wc || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/bin/head || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/bin/ls || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/bin/grep || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/ls.inst || exit 1
	@for f in ext/instructions/*.inst; do $(BUILD)/myfs $(DISK) put $$f /ext/instructions/$$(basename $$f) || exit 1; done
	@echo "==> заголовки в /include, библиотеки в /lib, исходники программ в /src (для cc)"
	@for d in /include /lib /lib/fonts /src /src/cc /etc /home /root /tmp /var /var/lib /var/log /usr /usr/bin /usr/lib /usr/share /usr/share/applications /mnt /media /proc /sys /run /ext /ext/bin /ext/instructions; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null 2>&1 || true; done
	@$(BUILD)/myfs $(DISK) mkdir /usr/share/wallpapers >/dev/null || exit 1
	@for f in wallpapers/*.jpg; do $(BUILD)/myfs $(DISK) put $$f /usr/share/wallpapers/$$(basename $$f) >/dev/null || exit 1; done
	@for f in apps/*.desktop; do $(BUILD)/myfs $(DISK) put $$f /usr/share/applications/$$(basename $$f) >/dev/null || exit 1; done
	@for f in myos.h sysinfo.h gfx.h win.h tls.h; do $(BUILD)/myfs $(DISK) put user/lib/$$f /include/$$f >/dev/null || exit 1; done
	@for f in user/include/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) mkdir /include/sys >/dev/null 2>&1 || true
	@for f in user/include/sys/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/sys/$$(basename $$f) >/dev/null || exit 1; done
	@for f in libc.c regex.c gfx.c vec.c font.c win.c tls.c; do $(BUILD)/myfs $(DISK) put user/lib/$$f /lib/$$f >/dev/null || exit 1; done
	@for f in $(MLIBS); do $(BUILD)/myfs $(DISK) put $$f /lib/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) put $(BUILD)/ld.so /lib/ld.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libc.so /lib/libc.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libgfx.so /lib/libgfx.so >/dev/null || exit 1
	@for f in user/code_*.h; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) put user/desktop_gpu.inc /src/desktop_gpu.inc >/dev/null || exit 1
	@for f in user/*.c; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@for f in user/cc/*.c user/cc/cc.h; do $(BUILD)/myfs $(DISK) put $$f /src/cc/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> шрифты Ubuntu в /lib/fonts"
	@for f in fonts/*.ttf; do $(BUILD)/myfs $(DISK) put $$f /lib/fonts/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> корневые сертификаты (список Mozilla) в /etc/certs.der"
	@$(BUILD)/myfs $(DISK) mkdir /etc >/dev/null 2>&1 || true
	@$(BUILD)/myfs $(DISK) put certs/roots.der /etc/certs.der >/dev/null

	@for d in /etc/services /etc/services/enabled; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null || exit 1; done
	@for f in services/*.service; do \
	  $(BUILD)/myfs $(DISK) cat /etc/services/$$(basename $$f) >/dev/null 2>&1 || \
	  $(BUILD)/myfs $(DISK) put $$f /etc/services/$$(basename $$f) >/dev/null || exit 1; done

# ------------------------------------------------------------------ запуск

# index=0 — primary master (загрузка), index=1 — primary slave (MyFS)
QEMU_DRIVES := -drive format=raw,file=$(BUILD)/os.img,index=0,media=disk \
               -drive format=raw,file=$(DISK),index=1,media=disk
QEMU_MEM ?= 256M
QEMU_CPUS ?= 4
# Optional independent data disk on secondary master: make run DATA_DISK=data.img
DATA_DISK ?=
ifneq ($(strip $(DATA_DISK)),)
QEMU_DRIVES += -drive format=raw,file=$(DATA_DISK),index=2,media=disk
endif
# auto: verified hardware VirGL on Linux/WSL, portable software framebuffer on Mac.
# GPU=on requires acceleration; GPU=off forces software; VGA=vmware/std overrides auto.
VGA ?= auto
GPU ?= auto
QEMU_VIDEO = python3 tools/run_qemu.py --qemu "$(QEMU)" --gpu "$(GPU)" --vga "$(VGA)" --
# сеть: карта e1000 за NAT QEMU; HOST_PORT хоста -> порт 80 MyOS (httpd)
# Другой порт: make run HOST_PORT=8081
HOST_PORT ?= 8080
QEMU_NET := -nic user,model=e1000,hostfwd=tcp::$(HOST_PORT)-:80 -rtc base=localtime

# всё, что пишет консоль ядра (и browser -v), — ещё и в build/serial.log
run: all
	$(QEMU_VIDEO) -smp $(QEMU_CPUS) -m $(QEMU_MEM) $(QEMU_DRIVES) $(QEMU_NET) -serial file:$(BUILD)/serial.log

debug: all
	$(QEMU_VIDEO) -smp $(QEMU_CPUS) $(QEMU_DRIVES) $(QEMU_NET) -no-reboot -d int,cpu_reset -s -S

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
	$(MAKE) $(BUILD)/myfs
	dd if=/dev/zero of=$(BUILD)/v1.img bs=512 count=4096 status=none
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/v1_seed.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/host_io.o -o $(BUILD)/v1_seed
	./$(BUILD)/v1_seed $(BUILD)/v1.img
	./$(BUILD)/myfs $(BUILD)/v1.img grow 4
	./$(BUILD)/myfs $(BUILD)/v1.img cat /keep/note | grep -q v1-ok
	./$(BUILD)/myfs $(BUILD)/v1.img ls /keep | grep -q '<LNK>'
	./$(BUILD)/myfs $(BUILD)/v1.img cat /keep/alias | grep -q v1-ok
	./$(BUILD)/myfs $(BUILD)/v1.img cat /$$(python3 -c 'print("n"*27)') | grep -q long
	python3 -c 'd=open("$(BUILD)/v1.img","rb").read(8); v=int.from_bytes(d[4:8],"little"); assert d[:4]==b"MYFS" and v==2, v'
	./$(BUILD)/myfs $(BUILD)/v1.img mkdir /$$(python3 -c 'print("d"*40)')
	./$(BUILD)/myfs $(BUILD)/v1.img mkdir /$$(python3 -c 'print("p"*180)')
	rm -f $(BUILD)/v1.img $(BUILD)/v1.img.bak
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
	python3 tests/modules_test.py

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
# Optimized host benchmark; sanitizer builds are used separately by browsertest.
$(BUILD)/browser-bench: tests/browser_host.c tests/browser_compat.h user/browser.c user/lib/dom_prelude.h $(wildcard user/lib/*.h) $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c user/lib/tls.c user/lib/x509.c user/lib/img.c user/lib/svg.c | $(BUILD)
	gcc -m32 -std=gnu17 -O2 -g -DMYOS_HOST_LIBC -fno-builtin -nostdinc -w -Iuser/lib \
	    tests/browser_host.c user/lib/win.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/tls.c user/lib/x509.c \
	    user/lib/img.c user/lib/svg.c $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $@
.PHONY: browserbench
browserbench: $(BUILD)/browser-bench
	./$(BUILD)/browser-bench tests/web/layout.html w 1920 h 600 bench 500

browsertest: user/lib/dom_prelude.h | $(BUILD)
	gcc -m32 -std=gnu17 -O1 -g -DMYOS_HOST_LIBC -fsanitize=address,undefined -fno-omit-frame-pointer -fno-builtin -nostdinc -w -Iuser/lib \
	    tests/browser_host.c user/lib/win.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/tls.c user/lib/x509.c \
	    user/lib/img.c user/lib/svg.c $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $(BUILD)/browser-host
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/t1.html scroll w 1280 scroll w 480 click keys timers 200
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/css.html scroll w 1000 images
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host --clipboard-test
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host --compat-test
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/modules.html assert-no-errors scroll
	UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/async-generators.html timers 150 assert-no-errors
	UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/web-api.html timers 250 assert-no-errors

# Сглаженная графика и шрифты на Linux: пробная картинка build/gfxtest.ppm
gfxtest: | $(BUILD)
	gcc -std=gnu17 -m32 -O1 -g -fno-builtin -nostdinc -w -Iuser/lib tests/gfx_host.c user/lib/gfx.c user/lib/vec.c \
	    user/lib/font.c -o $(BUILD)/gfx-host
	./$(BUILD)/gfx-host fonts > $(BUILD)/gfxtest.ppm && echo "==> $(BUILD)/gfxtest.ppm"

# Тесты компилятора cc на Linux: тестовые программы и «cc собирает сам себя»
cctest: $(BUILD)/cc-host $(CCROOT_READY)
	@echo 'int main(void) { return 0; }' > $(BUILD)/cct_probe.c
	@$(BUILD)/cc-host -B $(CCROOT) -linux $(BUILD)/cct_probe.c -o $(BUILD)/cct_probe
	@if ! ./$(BUILD)/cct_probe 2>/dev/null; then \
	    echo "32-bit programs cannot run here - only checking that tests and cc itself compile"; \
	    for t in tests/cc/*.c; do $(BUILD)/cc-host -B $(CCROOT) -linux $$t -o $(BUILD)/cct_x || exit 1; done; \
	    for t in tests/cc/*.c; do $(BUILD)/cc-host -B $(CCROOT) -linux -fPIC $$t -o $(BUILD)/cct_x || exit 1; done; \
	    $(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && \
	    sh tests/ccshared.sh $(BUILD)/cc-host $(CCROOT) && echo "compile-only: OK"; \
	    exit 0; \
	fi; \
	for t in tests/cc/*.c; do \
	    n=$$(basename $$t .c); \
	    $(BUILD)/cc-host -B $(CCROOT) -linux $$t -o $(BUILD)/cct_$$n && ./$(BUILD)/cct_$$n || exit 1; \
	done; \
	for t in tests/cc/*.c; do \
	    n=$$(basename $$t .c); \
	    $(BUILD)/cc-host -B $(CCROOT) -linux -fPIC $$t -o $(BUILD)/cct_pic_$$n && ./$(BUILD)/cct_pic_$$n || exit 1; \
	done; \
	echo "cc -fPIC: OK"; \
	rm -f $(BUILD)/cct_cc1 $(BUILD)/cct_cc2 $(BUILD)/cct_cc3; \
	$(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && \
	./$(BUILD)/cct_cc1 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc2 && \
	./$(BUILD)/cct_cc2 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc3 && \
	cmp $(BUILD)/cct_cc2 $(BUILD)/cct_cc3 && echo "cc compiles itself: OK (stage 2 == stage 3)" && \
	sh tests/cclink.sh $(BUILD)/cc-host $(CCROOT) && sh tests/cclink.sh $(BUILD)/cct_cc2 $(CCROOT) && \
	sh tests/cclib.sh $(BUILD)/cc-host $(CCROOT) && sh tests/cclib.sh $(BUILD)/cct_cc2 $(CCROOT) && \
	sh tests/ccshared.sh $(BUILD)/cc-host $(CCROOT) && sh tests/ccshared.sh $(BUILD)/cct_cc2 $(CCROOT)

vfstest: all
	python3 tests/vfs_smoke.py

largetest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/large-host-io.o
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/large_fs_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/large-host-io.o -o $(BUILD)/large-fs-test
	python3 tests/run_large_fs.py $(BUILD)/large-fs-test

# ext без сети: разбор инструкций, каталоги, отказ на неизвестной команде.
exttest: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/user/download $(BUILD)/user/ext
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/ext.c -o $(BUILD)/ext-linux
	@./$(BUILD)/ext-linux --selftest

# gzip на хосте: тот же cc, что работает внутри системы, плюс сверка с GNU gzip.
gztest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/gzip.c -o $(BUILD)/gzip-linux
	@sh tests/gzip.sh $(BUILD)/gzip-linux

# tar на хосте: тот же cc, синтетические архивы и настоящий исходник zlib.
tartest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/tar.c -o $(BUILD)/tar-linux
	@sh tests/tar.sh $(BUILD)/tar-linux

cptest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/cp.c -o $(BUILD)/cp-linux
	@sh tests/cp.sh $(BUILD)/cp-linux

$(BUILD)/user/gpuscene.o: tests/gpu_scene.c user/desktop.c user/desktop_gpu.inc user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/gpuscene: $(BUILD)/user/gpuscene.o $(BUILD)/user/l_img.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

$(BUILD)/user/mousecheck.o: tests/mousecheck.c user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/mousecheck: $(BUILD)/user/mousecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/inputlatency.o: tests/inputlatency.c user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/inputlatency: $(BUILD)/user/inputlatency.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: mousetest
mousetest:
	python3 tests/mouse_smoke.py

.PHONY: graphicstest
graphicstest:
	python3 tests/graphics_launcher_test.py
	python3 tests/graphics_smoke.py --backend gpu
	python3 tests/graphics_smoke.py --backend std
	python3 tests/graphics_smoke.py --backend virtio

$(BUILD)/user/gpucheck.o: tests/gpucheck.c user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/gpucheck: $(BUILD)/user/gpucheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

# Загрузчик и программа с .so: внутри QEMU, диск — копия.
dynsystest: $(BUILD)/ld.so $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so $(BUILD)/user/echo $(BUILD)/user/sh $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/os.img $(BUILD)/myfs
	gcc -m32 -shared -fPIC -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -mno-sse -mno-mmx -O2 \
	    -Wl,-soname,libgccadd.so -Wl,--hash-style=sysv -o $(BUILD)/libgccadd.so tests/dyn/gccadd.c
	$(BUILD)/cc-host -B $(CCROOT) -nostdlib -o $(BUILD)/cc-gccso tests/dyn/use_gcc.c $(BUILD)/libgccadd.so
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $(BUILD)/libadd.so tests/ldso/add.c
	$(NASM) -f elf32 tests/dyn/start.asm -o $(BUILD)/dynstart.o
	gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx -O2 \
	    -c tests/dyn/use_cc.c -o $(BUILD)/use_cc.o
	ld -m elf_i386 -nostdlib -dynamic-linker /lib/ld.so --hash-style=sysv -Ttext-segment=0x40000000 \
	    -o $(BUILD)/gcc-ccso $(BUILD)/dynstart.o $(BUILD)/use_cc.o $(BUILD)/libadd.so
	gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx -O2 \
	    -Iuser/include -Iuser/lib -c tests/dyn/gfxuse.c -o $(BUILD)/gfxuse.o
	ld $(DYNLDFLAGS) -o $(BUILD)/gfxuse $(BUILD)/user/crt0.o $(BUILD)/gfxuse.o -L$(BUILD)/user -lc -lgfx
	python3 tests/dynsys_smoke.py

ldsotest: $(BUILD)/ld.so $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/os.img $(BUILD)/myfs
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $(BUILD)/libadd.so tests/ldso/add.c
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $(BUILD)/libb.so tests/ldso/b.c $(BUILD)/libadd.so
	$(BUILD)/cc-host -B $(CCROOT) -nostdlib -o $(BUILD)/dynuse tests/ldso/main.c $(BUILD)/libadd.so $(BUILD)/ld.so
	python3 tests/ldso_smoke.py

.PHONY: all run debug clean cleandisk test programs install cctest jstest gfxtest vfstest largetest exttest gztest tartest cptest ldsotest dynsystest
.SECONDARY:



$(BUILD)/user/servicecheck.o: tests/servicecheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/servicecheck: $(BUILD)/user/servicecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: servicetest
servicetest:
	python3 tests/services_smoke.py

.PHONY: codetest
codetest: | $(BUILD)
	$(HOSTCC) -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined tests/code_features.c -o $(BUILD)/code-features-test
	./$(BUILD)/code-features-test user/lib/myos.h user/lib/gfx.h user/lib/win.h
	$(HOSTCC) -std=gnu11 -DMYOS_HOST_LIBC -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections -Iuser/lib tests/code_document.c -Wl,--gc-sections -o $(BUILD)/code-document-test
	./$(BUILD)/code-document-test

# Settings validates images; the desktop decodes the saved wallpaper.
$(BUILD)/user/desktop.o: user/desktop_gpu.inc

$(BUILD)/user/settings $(BUILD)/user/desktop: $(BUILD)/user/%: $(BUILD)/user/%.o $(BUILD)/user/l_img.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

.PHONY: settingstest
settingstest:
	python3 tests/settings_smoke.py

$(BUILD)/user/sysinfocheck.o: tests/sysinfocheck.c user/lib/myos.h user/lib/sysinfo.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/sysinfocheck: $(BUILD)/user/sysinfocheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: sysinfotest
sysinfotest:
	python3 tests/build_info_test.py
	python3 tests/sysinfo_smoke.py

# Hardware/USB live build is isolated from the original QEMU boot image.
HWBUILD := $(BUILD)/hardware
$(HWBUILD)/virtgpu.o: kernel/virtgpu_render.inc kernel/virtgpu_cursor.inc
HWOBJS := $(HWBUILD)/entry.o $(HWBUILD)/isr.o $(HWBUILD)/ap.o $(patsubst kernel/%.c,$(HWBUILD)/%.o,$(C_SRC))
$(HWBUILD):
	mkdir -p $@
$(HWBUILD)/%.o: kernel/%.c kernel/*.h $(BUILD)/build_info.h | $(HWBUILD)
	$(CC) $(CFLAGS) -DMYOS_HARDWARE -c $< -o $@
$(HWBUILD)/%.o: kernel/%.asm | $(HWBUILD)
	$(NASM) -f elf32 $< -o $@
$(HWBUILD)/kernel.elf: $(HWOBJS) boot/hardware.ld
	$(LD) -m elf_i386 -T boot/hardware.ld -nostdlib $(HWOBJS) -o $@
$(HWBUILD)/kernel.bin: $(HWBUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
.PHONY: usb-image usbtest
usb-image: $(HWBUILD)/kernel.bin $(UBINS) $(BUILD)/myfs
	python3 tools/make_usb_image.py --nasm $(NASM) --nm $(CROSS)nm
usbtest: usb-image
	python3 tests/hardware_smoke.py

.PHONY: htoptest
htoptest:
	$(MAKE) build/os.img install DISK=build/htop-test.img
	python3 tests/htop_smoke.py

$(BUILD)/user/cpuinfocheck.o: tests/cpuinfocheck.c user/lib/myos.h user/lib/sysinfo.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/cpuinfocheck: $(BUILD)/user/cpuinfocheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/terminalresizecheck.o: tests/terminalresizecheck.c user/terminal.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/terminalresizecheck: $(BUILD)/user/terminalresizecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: terminalresizetest
terminalresizetest:
	$(MAKE) build/os.img build/user/terminalresizecheck install DISK=build/terminal-resize-test.img
	python3 tests/terminal_resize_smoke.py

$(BUILD)/user/terminalwindowcheck.o: tests/terminalwindowcheck.c user/lib/myos.h user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/terminalwindowcheck: $(BUILD)/user/terminalwindowcheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@



.PHONY: terminalwindowtest
terminalwindowtest:
	$(MAKE) build/os.img build/user/terminalwindowcheck install DISK=build/terminal-resize-test.img
	python3 tests/terminal_window_smoke.py

# Mouse wheel: terminal history on host + PS/2 packets in QEMU.
.PHONY: wheeltest
wheeltest: $(BUILD)/os.img
	$(HOSTCC) -m32 -std=gnu11 -DMYOS_HOST_LIBC -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections -Iuser/lib tests/terminal_history.c -Wl,--gc-sections -o $(BUILD)/terminal-history-test
	./$(BUILD)/terminal-history-test
	python3 tests/wheel_smoke.py

# Native user threads and shared-address-space SMP regression tests.
$(BUILD)/user/threadcheck.o: tests/threadcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/threadcheck: $(BUILD)/user/threadcheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: threadtest
threadtest:
	python3 tests/thread_smoke.py
