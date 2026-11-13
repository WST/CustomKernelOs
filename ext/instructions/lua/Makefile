CC = cc -DLUA_USE_C89

all: /ext/bin/lua

/ext/bin/lua: lua.o liblua.a
	$(CC) -o /ext/bin/lua lua.o liblua.a

liblua.a: lapi.o lcode.o lctype.o ldebug.o ldo.o ldump.o lfunc.o lgc.o llex.o lmem.o lobject.o lopcodes.o lparser.o lstate.o lstring.o ltable.o ltm.o lundump.o lvm.o lzio.o lauxlib.o lbaselib.o lcorolib.o ldblib.o liolib.o lmathlib.o loadlib.o loslib.o lstrlib.o ltablib.o lutf8lib.o linit.o
	ar rc liblua.a lapi.o lcode.o lctype.o ldebug.o ldo.o ldump.o lfunc.o lgc.o
	ar rc liblua.a llex.o lmem.o lobject.o lopcodes.o lparser.o lstate.o lstring.o ltable.o
	ar rc liblua.a ltm.o lundump.o lvm.o lzio.o lauxlib.o lbaselib.o lcorolib.o ldblib.o
	ar rc liblua.a liolib.o lmathlib.o loadlib.o loslib.o lstrlib.o ltablib.o lutf8lib.o linit.o

%.o: %.c force
	$(CC) -c $< -o $@

.PHONY: all force
force:
