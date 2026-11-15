# CPython 3.15.2 для MyOS. configure не запускаем.
CC = cc -I. -IInclude -IInclude/internal
LIBS = -lpthread -lssl -lcrypto -lz

all: /ext/bin/python3 /ext/bin/virtualenv

p0.a: Python/Python-ast.o Python/Python-tokenize.o Python/_contextvars.o Python/_warnings.o Python/asdl.o Python/assemble.o Python/ast.o Python/ast_preprocess.o Python/ast_unparse.o Python/bltinmodule.o Python/bootstrap_hash.o Python/brc.o Python/ceval.o Python/ceval_gil.o Python/codecs.o Python/codegen.o Python/compile.o Python/context.o Python/critical_section.o Python/crossinterp.o Python/dtoa.o Python/dynamic_annotations.o Python/dynload_stub.o Python/errors.o Python/fileutils.o Python/flowgraph.o Python/frame.o Python/frozenmain.o Python/future.o Python/gc.o Python/gc_free_threading.o Python/gc_gil.o Python/getargs.o Python/getcompiler.o Python/getcopyright.o Python/getopt.o Python/getplatform.o Python/getversion.o Python/hamt.o
	ar rc p0.a Python/Python-ast.o Python/Python-tokenize.o Python/_contextvars.o Python/_warnings.o Python/asdl.o Python/assemble.o Python/ast.o Python/ast_preprocess.o Python/ast_unparse.o Python/bltinmodule.o Python/bootstrap_hash.o Python/brc.o Python/ceval.o Python/ceval_gil.o Python/codecs.o Python/codegen.o Python/compile.o Python/context.o Python/critical_section.o Python/crossinterp.o
	ar rc p0.a Python/dtoa.o Python/dynamic_annotations.o Python/dynload_stub.o Python/errors.o Python/fileutils.o Python/flowgraph.o Python/frame.o Python/frozenmain.o Python/future.o Python/gc.o Python/gc_free_threading.o Python/gc_gil.o Python/getargs.o Python/getcompiler.o Python/getcopyright.o Python/getopt.o Python/getplatform.o Python/getversion.o Python/hamt.o

p1.a: Python/hashtable.o Python/import.o Python/importdl.o Python/index_pool.o Python/initconfig.o Python/instruction_sequence.o Python/instrumentation.o Python/interpconfig.o Python/intrinsics.o Python/jit.o Python/jit_publish.o Python/jit_unwind.o Python/legacy_tracing.o Python/lock.o Python/marshal.o Python/modsupport.o Python/mysnprintf.o Python/mystrtoul.o Python/object_stack.o Python/optimizer.o Python/optimizer_analysis.o Python/optimizer_symbols.o Python/parking_lot.o Python/pathconfig.o Python/perf_jit_trampoline.o Python/perf_trampoline.o Python/preconfig.o Python/pyarena.o Python/pyctype.o Python/pyfpe.o Python/pyhash.o Python/pylifecycle.o Python/pymath.o Python/pystate.o Python/pystats.o Python/pystrcmp.o Python/pystrhex.o Python/pystrtod.o Python/pythonrun.o Python/pytime.o
	ar rc p1.a Python/hashtable.o Python/import.o Python/importdl.o Python/index_pool.o Python/initconfig.o Python/instruction_sequence.o Python/instrumentation.o Python/interpconfig.o Python/intrinsics.o Python/jit.o Python/jit_publish.o Python/jit_unwind.o Python/legacy_tracing.o Python/lock.o Python/marshal.o Python/modsupport.o Python/mysnprintf.o Python/mystrtoul.o Python/object_stack.o Python/optimizer.o
	ar rc p1.a Python/optimizer_analysis.o Python/optimizer_symbols.o Python/parking_lot.o Python/pathconfig.o Python/perf_jit_trampoline.o Python/perf_trampoline.o Python/preconfig.o Python/pyarena.o Python/pyctype.o Python/pyfpe.o Python/pyhash.o Python/pylifecycle.o Python/pymath.o Python/pystate.o Python/pystats.o Python/pystrcmp.o Python/pystrhex.o Python/pystrtod.o Python/pythonrun.o Python/pytime.o

p2.a: Python/qsbr.o Python/remote_debugging.o Python/slots.o Python/slots_generated.o Python/specialize.o Python/stackrefs.o Python/structmember.o Python/suggestions.o Python/symtable.o Python/sysmodule.o Python/thread.o Python/traceback.o Python/tracemalloc.o Python/uniqueid.o Objects/abstract.o Objects/boolobject.o Objects/bytearrayobject.o Objects/bytes_methods.o Objects/bytesobject.o Objects/call.o Objects/capsule.o Objects/cellobject.o Objects/classobject.o Objects/codeobject.o Objects/complexobject.o Objects/descrobject.o Objects/dictobject.o Objects/enumobject.o Objects/exceptions.o Objects/fileobject.o Objects/floatobject.o Objects/frameobject.o Objects/funcobject.o Objects/genericaliasobject.o Objects/genobject.o Objects/interpolationobject.o Objects/iterobject.o Objects/lazyimportobject.o Objects/listobject.o Objects/longobject.o
	ar rc p2.a Python/qsbr.o Python/remote_debugging.o Python/slots.o Python/slots_generated.o Python/specialize.o Python/stackrefs.o Python/structmember.o Python/suggestions.o Python/symtable.o Python/sysmodule.o Python/thread.o Python/traceback.o Python/tracemalloc.o Python/uniqueid.o Objects/abstract.o Objects/boolobject.o Objects/bytearrayobject.o Objects/bytes_methods.o Objects/bytesobject.o Objects/call.o
	ar rc p2.a Objects/capsule.o Objects/cellobject.o Objects/classobject.o Objects/codeobject.o Objects/complexobject.o Objects/descrobject.o Objects/dictobject.o Objects/enumobject.o Objects/exceptions.o Objects/fileobject.o Objects/floatobject.o Objects/frameobject.o Objects/funcobject.o Objects/genericaliasobject.o Objects/genobject.o Objects/interpolationobject.o Objects/iterobject.o Objects/lazyimportobject.o Objects/listobject.o Objects/longobject.o

p3.a: Objects/memoryobject.o Objects/methodobject.o Objects/moduleobject.o Objects/namespaceobject.o Objects/object.o Objects/obmalloc.o Objects/odictobject.o Objects/picklebufobject.o Objects/rangeobject.o Objects/sentinelobject.o Objects/setobject.o Objects/sliceobject.o Objects/structseq.o Objects/templateobject.o Objects/tupleobject.o Objects/typeobject.o Objects/typevarobject.o Objects/unicode_format.o Objects/unicode_formatter.o Objects/unicode_writer.o Objects/unicodectype.o Objects/unicodeobject.o Objects/unionobject.o Objects/weakrefobject.o Parser/action_helpers.o Parser/myreadline.o Parser/parser.o Parser/peg_api.o Parser/pegen.o Parser/pegen_errors.o Parser/string_parser.o Parser/token.o Parser/lexer/buffer.o Parser/lexer/lexer.o Parser/lexer/state.o Parser/tokenizer/file_tokenizer.o Parser/tokenizer/helpers.o Parser/tokenizer/readline_tokenizer.o Parser/tokenizer/string_tokenizer.o Parser/tokenizer/utf8_tokenizer.o
	ar rc p3.a Objects/memoryobject.o Objects/methodobject.o Objects/moduleobject.o Objects/namespaceobject.o Objects/object.o Objects/obmalloc.o Objects/odictobject.o Objects/picklebufobject.o Objects/rangeobject.o Objects/sentinelobject.o Objects/setobject.o Objects/sliceobject.o Objects/structseq.o Objects/templateobject.o Objects/tupleobject.o Objects/typeobject.o Objects/typevarobject.o Objects/unicode_format.o Objects/unicode_formatter.o Objects/unicode_writer.o
	ar rc p3.a Objects/unicodectype.o Objects/unicodeobject.o Objects/unionobject.o Objects/weakrefobject.o Parser/action_helpers.o Parser/myreadline.o Parser/parser.o Parser/peg_api.o Parser/pegen.o Parser/pegen_errors.o Parser/string_parser.o Parser/token.o Parser/lexer/buffer.o Parser/lexer/lexer.o Parser/lexer/state.o Parser/tokenizer/file_tokenizer.o Parser/tokenizer/helpers.o Parser/tokenizer/readline_tokenizer.o Parser/tokenizer/string_tokenizer.o Parser/tokenizer/utf8_tokenizer.o

p4.a: Modules/_io/_iomodule.o Modules/_io/bufferedio.o Modules/_io/bytesio.o Modules/_io/fileio.o Modules/_io/iobase.o Modules/_io/stringio.o Modules/_io/textio.o Modules/_io/winconsoleio.o Modules/posixmodule.o Modules/timemodule.o Modules/selectmodule.o Modules/_posixsubprocess.o Modules/socketmodule.o Modules/_ssl.o Modules/zlibmodule.o Modules/_hashopenssl.o Modules/fcntlmodule.o Modules/mathmodule.o Modules/mathintegermodule.o Modules/mmapmodule.o Modules/pyexpat.o Modules/binascii.o Modules/_struct.o Modules/unicodedata.o Modules/_json.o Modules/_sre/sre.o Modules/arraymodule.o Modules/_bisectmodule.o Modules/_csv.o Modules/_heapqmodule.o Modules/_pickle.o Modules/_randommodule.o Modules/gcmodule.o Modules/errnomodule.o Modules/pwdmodule.o Modules/grpmodule.o Modules/_threadmodule.o Modules/signalmodule.o Modules/atexitmodule.o Modules/_stat.o Modules/_weakref.o Modules/_codecsmodule.o Modules/_collectionsmodule.o
	ar rc p4.a Modules/_io/_iomodule.o Modules/_io/bufferedio.o Modules/_io/bytesio.o Modules/_io/fileio.o Modules/_io/iobase.o Modules/_io/stringio.o Modules/_io/textio.o Modules/_io/winconsoleio.o Modules/posixmodule.o Modules/timemodule.o Modules/selectmodule.o Modules/_posixsubprocess.o Modules/socketmodule.o Modules/_ssl.o Modules/zlibmodule.o Modules/_hashopenssl.o Modules/fcntlmodule.o Modules/mathmodule.o Modules/mathintegermodule.o Modules/mmapmodule.o Modules/pyexpat.o Modules/binascii.o Modules/_struct.o
	ar rc p4.a Modules/unicodedata.o Modules/_json.o Modules/_sre/sre.o Modules/arraymodule.o Modules/_bisectmodule.o Modules/_csv.o Modules/_heapqmodule.o Modules/_pickle.o Modules/_randommodule.o Modules/gcmodule.o Modules/errnomodule.o Modules/pwdmodule.o Modules/grpmodule.o Modules/_threadmodule.o Modules/signalmodule.o Modules/atexitmodule.o Modules/_stat.o Modules/_weakref.o Modules/_codecsmodule.o Modules/_collectionsmodule.o

p5.a: Modules/itertoolsmodule.o Modules/_functoolsmodule.o Modules/_operator.o Modules/symtablemodule.o Modules/_datetimemodule.o Modules/main.o Modules/getbuildinfo.o Modules/faulthandler.o Modules/_tracemalloc.o Modules/_suggestions.o Modules/_sysconfig.o Modules/_typesmodule.o Modules/_typingmodule.o Modules/_abc.o Modules/_localemodule.o Modules/_opcode.o Modules/config.o
	ar rc p5.a Modules/itertoolsmodule.o Modules/_functoolsmodule.o Modules/_operator.o Modules/symtablemodule.o Modules/_datetimemodule.o Modules/main.o Modules/getbuildinfo.o Modules/faulthandler.o Modules/_tracemalloc.o Modules/_suggestions.o Modules/_sysconfig.o Modules/_typesmodule.o Modules/_typingmodule.o Modules/_abc.o Modules/_localemodule.o Modules/_opcode.o Modules/config.o

_freeze_module: Programs/_freeze_module.o Modules/getpath_noop.o p0.a p1.a p2.a p3.a p4.a p5.a
	$(CC) -o _freeze_module Programs/_freeze_module.o Modules/getpath_noop.o p0.a p1.a p2.a p3.a p4.a p5.a $(LIBS)

Python/frozen_modules/importlib._bootstrap.h: _freeze_module Lib/importlib/_bootstrap.py
	./_freeze_module importlib._bootstrap Lib/importlib/_bootstrap.py Python/frozen_modules/importlib._bootstrap.h

Python/frozen_modules/importlib._bootstrap_external.h: _freeze_module Lib/importlib/_bootstrap_external.py
	./_freeze_module importlib._bootstrap_external Lib/importlib/_bootstrap_external.py Python/frozen_modules/importlib._bootstrap_external.h

Python/frozen_modules/zipimport.h: _freeze_module Lib/zipimport.py
	./_freeze_module zipimport Lib/zipimport.py Python/frozen_modules/zipimport.h

Python/frozen_modules/abc.h: _freeze_module Lib/abc.py
	./_freeze_module abc Lib/abc.py Python/frozen_modules/abc.h

Python/frozen_modules/codecs.h: _freeze_module Lib/codecs.py
	./_freeze_module codecs Lib/codecs.py Python/frozen_modules/codecs.h

Python/frozen_modules/io.h: _freeze_module Lib/io.py
	./_freeze_module io Lib/io.py Python/frozen_modules/io.h

Python/frozen_modules/_collections_abc.h: _freeze_module Lib/_collections_abc.py
	./_freeze_module _collections_abc Lib/_collections_abc.py Python/frozen_modules/_collections_abc.h

Python/frozen_modules/_sitebuiltins.h: _freeze_module Lib/_sitebuiltins.py
	./_freeze_module _sitebuiltins Lib/_sitebuiltins.py Python/frozen_modules/_sitebuiltins.h

Python/frozen_modules/genericpath.h: _freeze_module Lib/genericpath.py
	./_freeze_module genericpath Lib/genericpath.py Python/frozen_modules/genericpath.h

Python/frozen_modules/ntpath.h: _freeze_module Lib/ntpath.py
	./_freeze_module ntpath Lib/ntpath.py Python/frozen_modules/ntpath.h

Python/frozen_modules/posixpath.h: _freeze_module Lib/posixpath.py
	./_freeze_module posixpath Lib/posixpath.py Python/frozen_modules/posixpath.h

Python/frozen_modules/os.h: _freeze_module Lib/os.py
	./_freeze_module os Lib/os.py Python/frozen_modules/os.h

Python/frozen_modules/site.h: _freeze_module Lib/site.py
	./_freeze_module site Lib/site.py Python/frozen_modules/site.h

Python/frozen_modules/stat.h: _freeze_module Lib/stat.py
	./_freeze_module stat Lib/stat.py Python/frozen_modules/stat.h

Python/frozen_modules/linecache.h: _freeze_module Lib/linecache.py
	./_freeze_module linecache Lib/linecache.py Python/frozen_modules/linecache.h

Python/frozen_modules/importlib.util.h: _freeze_module Lib/importlib/util.py
	./_freeze_module importlib.util Lib/importlib/util.py Python/frozen_modules/importlib.util.h

Python/frozen_modules/importlib.machinery.h: _freeze_module Lib/importlib/machinery.py
	./_freeze_module importlib.machinery Lib/importlib/machinery.py Python/frozen_modules/importlib.machinery.h

Python/frozen_modules/runpy.h: _freeze_module Lib/runpy.py
	./_freeze_module runpy Lib/runpy.py Python/frozen_modules/runpy.h

Python/frozen_modules/__hello__.h: _freeze_module Lib/__hello__.py
	./_freeze_module __hello__ Lib/__hello__.py Python/frozen_modules/__hello__.h

Python/frozen_modules/__phello__.h: _freeze_module Lib/__phello__/__init__.py
	./_freeze_module __phello__ Lib/__phello__/__init__.py Python/frozen_modules/__phello__.h

Python/frozen_modules/__phello__.ham.h: _freeze_module Lib/__phello__/ham/__init__.py
	./_freeze_module __phello__.ham Lib/__phello__/ham/__init__.py Python/frozen_modules/__phello__.ham.h

Python/frozen_modules/__phello__.ham.eggs.h: _freeze_module Lib/__phello__/ham/eggs.py
	./_freeze_module __phello__.ham.eggs Lib/__phello__/ham/eggs.py Python/frozen_modules/__phello__.ham.eggs.h

Python/frozen_modules/__phello__.spam.h: _freeze_module Lib/__phello__/spam.py
	./_freeze_module __phello__.spam Lib/__phello__/spam.py Python/frozen_modules/__phello__.spam.h

Python/frozen_modules/frozen_only.h: _freeze_module Tools/freeze/flag.py
	./_freeze_module frozen_only Tools/freeze/flag.py Python/frozen_modules/frozen_only.h

Python/frozen_modules/getpath.h: _freeze_module Modules/getpath.py
	./_freeze_module getpath Modules/getpath.py Python/frozen_modules/getpath.h

Python/frozen.o: Python/frozen.c Python/frozen_modules/importlib._bootstrap.h Python/frozen_modules/importlib._bootstrap_external.h Python/frozen_modules/zipimport.h Python/frozen_modules/abc.h Python/frozen_modules/codecs.h Python/frozen_modules/io.h Python/frozen_modules/_collections_abc.h Python/frozen_modules/_sitebuiltins.h Python/frozen_modules/genericpath.h Python/frozen_modules/ntpath.h Python/frozen_modules/posixpath.h Python/frozen_modules/os.h Python/frozen_modules/site.h Python/frozen_modules/stat.h Python/frozen_modules/linecache.h Python/frozen_modules/importlib.util.h Python/frozen_modules/importlib.machinery.h Python/frozen_modules/runpy.h Python/frozen_modules/__hello__.h Python/frozen_modules/__phello__.h Python/frozen_modules/__phello__.ham.h Python/frozen_modules/__phello__.ham.eggs.h Python/frozen_modules/__phello__.spam.h Python/frozen_modules/frozen_only.h
	$(CC) -c Python/frozen.c -o Python/frozen.o

Modules/getpath.o: Modules/getpath.c Python/frozen_modules/getpath.h
	$(CC) -c Modules/getpath.c -o Modules/getpath.o -DPREFIX='"/ext"' -DEXEC_PREFIX='"/ext"' -DVERSION='"3.15"' -DVPATH='"."' -DPLATLIBDIR='"lib"'

pyinstall: pyinstall.c
	$(CC) -o pyinstall pyinstall.c

/ext/bin/python3: Programs/python.o Modules/getpath.o Python/frozen.o pyinstall p0.a p1.a p2.a p3.a p4.a p5.a
	$(CC) -o /ext/bin/python3 Programs/python.o Modules/getpath.o Python/frozen.o p0.a p1.a p2.a p3.a p4.a p5.a $(LIBS)
	./pyinstall
	/ext/bin/python3 -m ensurepip --default-pip

/ext/bin/virtualenv: /ext/bin/python3
	/ext/bin/python3 -m pip install --no-index --find-links /tmp/ext/python virtualenv

%.o: %.c
	$(CC) -c $< -o $@

.PHONY: all
