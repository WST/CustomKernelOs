# Драйверы видеокарт

`video.h` — внутренний интерфейс подключения видеодрайверов.
`firmware.c` — проверка и принятие framebuffer от загрузчика BIOS VBE.
`nvidia.c` — начальный firmware-backed backend NVIDIA; распознаёт
GTX 1660 SUPER (PCI 10de:21c4), проверяет принадлежность LFB через BAR1,
сохраняет установленный BIOS режим. На гибридной системе без совпадения BAR1
используется общее имя BIOS VBE, а найденная NVIDIA только пишется в журнал.

Модули собираются в обычное и аппаратное ядро через Makefile. Рисование,
software cursor и отображение с PAT WC пока обслуживает `kernel/fb.c`.
Существующие VirtIO, VMware и Bochs backend остаются в kernel; новые драйверы
следует добавлять сюда. Нет native modesetting, 3D и собственного управления HDMI.
`runtime.c` выполняет видеобиос для переключения режимов внутри ОС,
`x86emu/` содержит BSD-совместимый интерпретатор с сохранённой лицензией.
Меню V выбирает начальный режим. Нативное ускорение Turing ещё не реализовано.

[Выбор разрешения и проверка](../../docs/nvidia.md).
[NVIDIA PCI ID](https://download.nvidia.com/XFree86/Linux-x86_64/450.57/README/supportedchips.html).
[Документация аппаратных интерфейсов NVIDIA](https://github.com/NVIDIA/open-gpu-doc).
