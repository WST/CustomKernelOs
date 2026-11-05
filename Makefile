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

CC      := gcc
LD      := ld
NASM    := nasm
OBJCOPY := objcopy
QEMU    := qemu-system-i386

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
	$(NASM) -f bin -DKERNEL_SECTORS=$$(( ($$(stat -c %s $(BUILD)/kernel.bin) + 511) / 512 )) $< -o $@

# Образ: [сектор 0: boot.bin][сектора 1..N: kernel.bin], дополнен до 1.44 МБ
$(BUILD)/os.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin
	cat $^ > $@
	truncate -s 1474560 $@
	@echo "==> $@ готов (ядро: $$(stat -c %s $(BUILD)/kernel.bin) байт)"

# ------------------------------------------------------------------ программы (ring 3)

# программам FPU можно (ядро сохраняет его регистры при переключении), SSE — нет
UCFLAGS  := -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
            -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx \
            -O2 -Wall -std=gnu11 -Iuser/lib -ffunction-sections -fdata-sections
# --gc-sections и библиотека-архив: в программу попадает только то, что она зовёт
ULDFLAGS := -m elf_i386 -T user/user.ld -nostdlib -s --gc-sections
PROGRAMS := $(basename $(notdir $(wildcard user/*.c))) cc
UBINS    := $(addprefix $(BUILD)/user/,$(PROGRAMS))

ULIB_OBJS := $(BUILD)/user/libc.o $(BUILD)/user/gfx.o $(BUILD)/user/vec.o $(BUILD)/user/font.o $(BUILD)/user/win.o
ULIB := $(BUILD)/user/libmyos.a

$(ULIB): $(ULIB_OBJS)
	rm -f $@ && ar rcs $@ $^

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

$(BUILD)/user/tls.o: user/lib/tls.c user/lib/tls.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(patsubst %,%.o,$(TLS_PROGS)) $(BUILD)/user/browser.o: user/lib/tls.h

$(TLS_PROGS): $(BUILD)/user/%: $(BUILD)/user/%.o $(ULIB) $(BUILD)/user/tls.o user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/tls.o $(ULIB) -o $@

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

$(BUILD)/user/browser: $(BUILD)/user/browser.o $(ULIB) $(BUILD)/user/tls.o $(JS_OBJS) $(DOM_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/tls.o $(JS_OBJS) $(DOM_OBJS) $(ULIB) -o $@

# cc — компилятор Си, который работает внутри MyOS (несколько файлов в user/cc/)
CC_SRC  := $(wildcard user/cc/*.c)
CC_OBJS := $(patsubst user/cc/%.c,$(BUILD)/user/cc_%.o,$(CC_SRC))

$(BUILD)/user/cc_%.o: user/cc/%.c user/cc/cc.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/cc: $(CC_OBJS) $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $(CC_OBJS) $(ULIB) -o $@

# cc для Linux — чтобы быстро гонять тесты компилятора (make cctest)
$(BUILD)/cc-host: $(CC_SRC) user/cc/cc.h | $(BUILD)
	gcc -std=gnu17 -DCC_HOST -O1 -g -Wall -Wno-unused-function -o $@ $(CC_SRC)

programs: $(UBINS)

# ------------------------------------------------------------------ утилита myfs (хост)

$(BUILD)/myfs: tools/myfs.c tools/host_io.c kernel/fs.c kernel/fs.h kernel/string.c | $(BUILD)
	gcc -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/host_io.o
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tools/myfs.c kernel/fs.c kernel/string.c \
	    $(BUILD)/host_io.o -o $@

# Диск: если его нет — создаём пустой (myfs сам его отформатирует)
$(DISK):
	truncate -s $(DISK_MB)M $@

# Установить программы в /bin (остальные файлы на диске не трогаются)
install: $(UBINS) $(BUILD)/myfs | $(DISK)
	@echo "==> устанавливаю программы в /bin на $(DISK)"
	@for p in $(PROGRAMS); do $(BUILD)/myfs $(DISK) put $(BUILD)/user/$$p /bin/$$p || exit 1; done
	@echo "==> заголовки в /include, библиотеки в /lib, исходники программ в /src (для cc)"
	@for d in /include /lib /lib/fonts /src /src/cc; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null 2>&1 || true; done
	@for f in myos.h gfx.h win.h tls.h; do $(BUILD)/myfs $(DISK) put user/lib/$$f /include/$$f >/dev/null || exit 1; done
	@for f in libc.c gfx.c vec.c font.c win.c tls.c; do $(BUILD)/myfs $(DISK) put user/lib/$$f /lib/$$f >/dev/null || exit 1; done
	@for f in user/*.c; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@for f in user/cc/*.c user/cc/cc.h; do $(BUILD)/myfs $(DISK) put $$f /src/cc/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> шрифты Ubuntu в /lib/fonts"
	@for f in fonts/*.ttf; do $(BUILD)/myfs $(DISK) put $$f /lib/fonts/$$(basename $$f) >/dev/null || exit 1; done

# ------------------------------------------------------------------ запуск

# index=0 — primary master (загрузка), index=1 — primary slave (MyFS)
QEMU_DRIVES := -drive format=raw,file=$(BUILD)/os.img,index=0,media=disk \
               -drive format=raw,file=$(DISK),index=1,media=disk
QEMU_MEM ?= 128M
# видеокарта: vmware — VMware SVGA II с ускорением 2D и аппаратным курсором (по умолчанию),
# std — простая Bochs VBE: make run VGA=std
VGA ?= vmware
# сеть: карта e1000 за NAT QEMU; порт 8080 Windows -> порт 80 MyOS (httpd)
QEMU_NET := -nic user,model=e1000,hostfwd=tcp::8080-:80 -rtc base=localtime

run: all
	$(QEMU) -m $(QEMU_MEM) -vga $(VGA) $(QEMU_DRIVES) $(QEMU_NET)

debug: all
	$(QEMU) -vga $(VGA) $(QEMU_DRIVES) $(QEMU_NET) -no-reboot -d int,cpu_reset -s -S

clean:
	rm -rf $(BUILD)

cleandisk:
	rm -f $(DISK)

# Юнит-тесты (запускаются на хосте, не в QEMU)
test: | $(BUILD)
	gcc -std=gnu17 -O1 -Wall -Wno-unused-function -Iuser/lib tests/tls_test.c -o $(BUILD)/tls_test
	./$(BUILD)/tls_test
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/fs_test.c kernel/fs.c kernel/string.c -o $(BUILD)/fs_test
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

# Сглаженная графика и шрифты на Linux: пробная картинка build/gfxtest.ppm
gfxtest: | $(BUILD)
	gcc -std=gnu17 -m32 -O1 -g -fno-builtin -nostdinc -w -Iuser/lib tests/gfx_host.c user/lib/gfx.c user/lib/vec.c \
	    user/lib/font.c -o $(BUILD)/gfx-host
	./$(BUILD)/gfx-host fonts > $(BUILD)/gfxtest.ppm && echo "==> $(BUILD)/gfxtest.ppm"

# Тесты компилятора cc на Linux: тестовые программы и «cc собирает сам себя»
CCROOT := $(BUILD)/ccroot
cctest: $(BUILD)/cc-host
	@mkdir -p $(CCROOT)/include $(CCROOT)/lib
	@cp user/lib/myos.h user/lib/gfx.h user/lib/win.h $(CCROOT)/include/
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
	$(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && \
	./$(BUILD)/cct_cc1 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc2 && \
	./$(BUILD)/cct_cc2 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc3 && \
	cmp $(BUILD)/cct_cc2 $(BUILD)/cct_cc3 && echo "cc compiles itself: OK (stage 2 == stage 3)"

.PHONY: all run debug clean cleandisk test programs install cctest jstest gfxtest
.SECONDARY:
