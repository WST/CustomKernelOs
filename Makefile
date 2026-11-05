# MyOS — сборка
#
#   make           — собрать ядро (build/os.img), программы и установить их в /bin на disk.img
#   make run       — всё собрать и запустить в QEMU
#   make test      — юнит-тесты файловой системы и кучи (на хосте)
#   make clean     — удалить build/ (диск с файлами disk.img НЕ трогается)
#   make cleandisk — удалить disk.img (при следующем запуске ФС создастся заново)
#
#   build/myfs disk.img ls /bin            — посмотреть диск с компьютера
#   build/myfs disk.img put file.txt /f    — положить свой файл на диск

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
            -O2 -Wall -std=gnu11 -Iuser/lib
ULDFLAGS := -m elf_i386 -T user/user.ld -nostdlib -s
PROGRAMS := $(basename $(notdir $(wildcard user/*.c))) cc
UBINS    := $(addprefix $(BUILD)/user/,$(PROGRAMS))

ULIB := $(BUILD)/user/libc.o $(BUILD)/user/gfx.o $(BUILD)/user/win.o

$(BUILD)/user/libc.o: user/lib/libc.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/gfx.o: user/lib/gfx.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/win.o: user/lib/win.c user/lib/win.h user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: user/%.c user/lib/myos.h user/lib/gfx.h user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/%: $(BUILD)/user/%.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $(ULIB) $< -o $@

# cc — компилятор Си, который работает внутри MyOS (несколько файлов в user/cc/)
CC_SRC  := $(wildcard user/cc/*.c)
CC_OBJS := $(patsubst user/cc/%.c,$(BUILD)/user/cc_%.o,$(CC_SRC))

$(BUILD)/user/cc_%.o: user/cc/%.c user/cc/cc.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/cc: $(CC_OBJS) $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $(ULIB) $(CC_OBJS) -o $@

# cc для Linux — чтобы быстро гонять тесты компилятора (make cctest)
$(BUILD)/cc-host: $(CC_SRC) user/cc/cc.h | $(BUILD)
	gcc -DCC_HOST -O1 -g -Wall -Wno-unused-function -o $@ $(CC_SRC)

programs: $(UBINS)

# ------------------------------------------------------------------ утилита myfs (хост)

$(BUILD)/myfs: tools/myfs.c tools/host_io.c kernel/fs.c kernel/fs.h kernel/string.c | $(BUILD)
	gcc -O1 -c tools/host_io.c -o $(BUILD)/host_io.o
	gcc -O1 -w -fno-builtin -DHOST_TEST -Ikernel tools/myfs.c kernel/fs.c kernel/string.c \
	    $(BUILD)/host_io.o -o $@

# Диск: если его нет — создаём пустой (myfs сам его отформатирует)
$(DISK):
	truncate -s $(DISK_MB)M $@

# Установить программы в /bin (остальные файлы на диске не трогаются)
install: $(UBINS) $(BUILD)/myfs | $(DISK)
	@echo "==> устанавливаю программы в /bin на $(DISK)"
	@for p in $(PROGRAMS); do $(BUILD)/myfs $(DISK) put $(BUILD)/user/$$p /bin/$$p || exit 1; done
	@echo "==> заголовки в /include, библиотеки в /lib, исходники программ в /src (для cc)"
	@for d in /include /lib /src /src/cc; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null 2>&1 || true; done
	@for f in myos.h gfx.h win.h; do $(BUILD)/myfs $(DISK) put user/lib/$$f /include/$$f >/dev/null || exit 1; done
	@for f in libc.c gfx.c win.c; do $(BUILD)/myfs $(DISK) put user/lib/$$f /lib/$$f >/dev/null || exit 1; done
	@for f in user/*.c; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@for f in user/cc/*.c user/cc/cc.h; do $(BUILD)/myfs $(DISK) put $$f /src/cc/$$(basename $$f) >/dev/null || exit 1; done

# ------------------------------------------------------------------ запуск

# index=0 — primary master (загрузка), index=1 — primary slave (MyFS)
QEMU_DRIVES := -drive format=raw,file=$(BUILD)/os.img,index=0,media=disk \
               -drive format=raw,file=$(DISK),index=1,media=disk
QEMU_MEM ?= 128M
# сеть: карта e1000 за NAT QEMU; порт 8080 Windows -> порт 80 MyOS (httpd)
QEMU_NET := -nic user,model=e1000,hostfwd=tcp::8080-:80 -rtc base=localtime

run: all
	$(QEMU) -m $(QEMU_MEM) $(QEMU_DRIVES) $(QEMU_NET)

debug: all
	$(QEMU) $(QEMU_DRIVES) $(QEMU_NET) -no-reboot -d int,cpu_reset -s -S

clean:
	rm -rf $(BUILD)

cleandisk:
	rm -f $(DISK)

# Юнит-тесты (запускаются на хосте, не в QEMU)
test: | $(BUILD)
	gcc -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/fs_test.c kernel/fs.c kernel/string.c -o $(BUILD)/fs_test
	./$(BUILD)/fs_test
	gcc -O1 -w -fno-builtin -Ikernel -include tests/host_env.h \
	    tests/heap_test.c kernel/heap.c kernel/string.c -o $(BUILD)/heap_test
	./$(BUILD)/heap_test

# Тесты компилятора cc на Linux: тестовые программы и «cc собирает сам себя»
CCROOT := $(BUILD)/ccroot
cctest: $(BUILD)/cc-host
	@mkdir -p $(CCROOT)/include $(CCROOT)/lib
	@cp user/lib/myos.h user/lib/gfx.h user/lib/win.h $(CCROOT)/include/
	@cp user/lib/libc.c user/lib/gfx.c user/lib/win.c $(CCROOT)/lib/
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

.PHONY: all run debug clean cleandisk test programs install cctest
.SECONDARY:
