# CPython 3.15.2 через ext

`ext install python` скачивает gzip-архив CPython 3.15.2 и собирает его
компилятором `cc` внутри MyOS. `configure` не запускается: в рецепте уже
лежат `pyconfig.h` и Makefile. Префикс — `/ext`.

```sh
ext install python
python3 -c 'print(1+1)'
python3 -m pip --version
python3 -m venv /tmp/v
virtualenv /tmp/e
```

Ставятся `/ext/bin/python3`, pip из `ensurepip` и virtualenv 20.31.2
с колёсами `distlib`, `filelock` и `platformdirs`. Каталог стандартной
библиотеки — `/ext/lib/python3.15`. В него не копируются `test`, `tkinter`
и `idlelib`. `ext uninstall python` удаляет интерпретатор, pip, virtualenv
и этот каталог.

Сборка идёт `make -j1`: один процесс `cc` и так занимает много памяти
(`Python/ceval.c` на хостовом прогоне — около 160 МиБ, это внутри предела
1 ГиБ). Сам make на время строки рецепта не остаётся второй копией. Замороженные модули создаёт `_freeze_module` уже во время `make`,
в репозиторий они не входят. Свежего тома на 64 МиБ не хватает: рецепт
рассчитан на выращенный диск (штатный `disk.img` — 4 ГиБ; новый том этого
размера получает 262144 inode, старый образ хранит прежнее число до `myfs grow`).

Профиль — i386 без свободных потоков, без JIT и без сторонних C-расширений.
`_ssl` и `_hashlib` линкуются с уже установленными `/lib/libssl.so` и
`/lib/libcrypto.so`, сертификаты читаются из `/etc/ssl/cert.pem`.

Проверка на хосте и в QEMU:

```sh
make python-build
make pythontest
```
