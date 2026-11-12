CC = cc -DHAVE_CONFIG_H -I. -Imyos-include -Isrc

all: /ext/bin/nano

/ext/bin/nano: browser.o chars.o color.o cut.o files.o global.o help.o history.o move.o nano.o prompt.o rcfile.o search.o text.o utils.o winio.o myos-posix.o myos-curses.o
	$(CC) browser.o chars.o color.o cut.o files.o global.o help.o history.o move.o nano.o prompt.o rcfile.o search.o text.o utils.o winio.o myos-posix.o myos-curses.o -o /ext/bin/nano
	cp COPYING /ext/share/nano/COPYING

browser.o: src/browser.c config.h myos-posix.h
	$(CC) -c src/browser.c -o browser.o

chars.o: src/chars.c config.h myos-posix.h
	$(CC) -c src/chars.c -o chars.o

color.o: src/color.c config.h myos-posix.h
	$(CC) -c src/color.c -o color.o

cut.o: src/cut.c config.h myos-posix.h
	$(CC) -c src/cut.c -o cut.o

files.o: src/files.c config.h myos-posix.h
	$(CC) -c src/files.c -o files.o

global.o: src/global.c config.h myos-posix.h
	$(CC) -c src/global.c -o global.o

help.o: src/help.c config.h myos-posix.h
	$(CC) -c src/help.c -o help.o

history.o: src/history.c config.h myos-posix.h
	$(CC) -c src/history.c -o history.o

move.o: src/move.c config.h myos-posix.h
	$(CC) -c src/move.c -o move.o

nano.o: src/nano.c config.h myos-posix.h
	$(CC) -c src/nano.c -o nano.o

prompt.o: src/prompt.c config.h myos-posix.h
	$(CC) -c src/prompt.c -o prompt.o

rcfile.o: src/rcfile.c config.h myos-posix.h
	$(CC) -c src/rcfile.c -o rcfile.o

search.o: src/search.c config.h myos-posix.h
	$(CC) -c src/search.c -o search.o

text.o: src/text.c config.h myos-posix.h
	$(CC) -c src/text.c -o text.o

utils.o: src/utils.c config.h myos-posix.h
	$(CC) -c src/utils.c -o utils.o

winio.o: src/winio.c config.h myos-posix.h
	$(CC) -c src/winio.c -o winio.o

myos-posix.o: myos-posix.c config.h myos-posix.h
	$(CC) -c myos-posix.c -o myos-posix.o

myos-curses.o: myos-curses.c config.h myos-posix.h
	$(CC) -c myos-curses.c -o myos-curses.o
