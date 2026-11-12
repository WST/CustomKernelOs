all: remove
	./remove

remove: remove.c
	cc remove.c -o remove

.PHONY: all
