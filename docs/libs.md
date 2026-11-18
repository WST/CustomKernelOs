# Библиотеки базовой системы

Заголовки лежат в `/include`, динамические библиотеки и статические архивы —
в `/lib`. `cc -lимя` находит и то и другое. Это свои подмножества с обычными
именами вызовов, не деревья upstream. `libintl` — заглушка.

| Файл | Заголовки | Чем отличается от полного варианта | Кто вызывает |
| --- | --- | --- | --- |
| `libz.so` | `zlib.h` | deflate для gzip, PNG и tar | `gzip`, `tar`, `libpng` |
| `libbz2.so` | `bzlib.h` | поток и буфер bzip2, без `bzip2recover` | `bzip2`, `bunzip2`, `tar -j` |
| `liblzma.so` | `lzma.h` | контейнер `.xz` и фильтр LZMA2 | `xz`, `unxz`, `tar -J` |
| `libiconv.so` | `iconv.h` | UTF-8/16/32, Latin-1, CP1251, KOI8-R, ASCII | `iconv` |
| `libpcre.so` | `pcre.h` | классический PCRE без lookaround и Unicode-свойств | `grep -P` |
| `libncurses.so` | `curses.h`, `ncurses.h`, `term.h` | экран в памяти и ANSI вместо базы terminfo | порт nano |
| `libintl.so` | `libintl.h` | `gettext` возвращает исходную строку, каталоги не читаются | порты, которым нужен `-lintl` |
| `libpng.so` | `png.h`, `pngconf.h` | 8 бит на запись; при чтении 1/2/4/16 бит раскрываются в 8 | `img`, `gd` |
| `libjpeg.so` | `jpeglib.h`, `jconfig.h`, `jmorecfg.h` | baseline и progressive на чтение, baseline 4:4:4 на запись | `img`, `gd` |
| `libgd.so` | `gd.h` | только truecolor, PNG и JPEG | `paint` |
| `libxml2.so` | `libxml/parser.h`, `libxml/tree.h` | документ из памяти и обход дерева, без XPath и DTD | разбор SVG |

`tar` при создании архива не сжимает поток: флаги `-z`, `-j` и `-J` на `tar c`
принимаются и пишут обычный ustar. Чтение `.tar.gz`, `.tar.bz2` и `.tar.xz`
идёт по сигнатуре и по явному флагу.
