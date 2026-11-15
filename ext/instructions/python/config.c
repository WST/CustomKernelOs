/* Встроенные модули. makesetup не запускаем: таблица собрана вручную. */
#include "Python.h"

extern PyObject *PyMarshal_Init(void);
extern PyObject *PyInit__imp(void);
extern PyObject *PyInit_gc(void);
extern PyObject *PyInit__ast(void);
extern PyObject *PyInit__tokenize(void);
extern PyObject *PyInit__contextvars(void);
extern PyObject *_PyWarnings_Init(void);
extern PyObject *PyInit__string(void);
extern PyObject *PyInit_posix(void);
extern PyObject *PyInit_errno(void);
extern PyObject *PyInit_pwd(void);
extern PyObject *PyInit_grp(void);
extern PyObject *PyInit__signal(void);
extern PyObject *PyInit__thread(void);
extern PyObject *PyInit_time(void);
extern PyObject *PyInit__math_integer(void);
extern PyObject *PyInit_math(void);
extern PyObject *PyInit_mmap(void);
extern PyObject *PyInit_pyexpat(void);
extern PyObject *PyInit_array(void);
extern PyObject *PyInit__bisect(void);
extern PyObject *PyInit__csv(void);
extern PyObject *PyInit__heapq(void);
extern PyObject *PyInit__json(void);
extern PyObject *PyInit__pickle(void);
extern PyObject *PyInit__random(void);
extern PyObject *PyInit__struct(void);
extern PyObject *PyInit_binascii(void);
extern PyObject *PyInit_zlib(void);
extern PyObject *PyInit_fcntl(void);
extern PyObject *PyInit_select(void);
extern PyObject *PyInit__socket(void);
extern PyObject *PyInit__ssl(void);
extern PyObject *PyInit__hashlib(void);
extern PyObject *PyInit_unicodedata(void);
extern PyObject *PyInit__posixsubprocess(void);
extern PyObject *PyInit__sre(void);
extern PyObject *PyInit__io(void);
extern PyObject *PyInit_atexit(void);
extern PyObject *PyInit__stat(void);
extern PyObject *PyInit__weakref(void);
extern PyObject *PyInit__codecs(void);
extern PyObject *PyInit__collections(void);
extern PyObject *PyInit_itertools(void);
extern PyObject *PyInit__functools(void);
extern PyObject *PyInit__operator(void);
extern PyObject *PyInit__datetime(void);
extern PyObject *PyInit__symtable(void);
extern PyObject *PyInit_faulthandler(void);
extern PyObject *PyInit__tracemalloc(void);
extern PyObject *PyInit__suggestions(void);
extern PyObject *PyInit__sysconfig(void);
extern PyObject *PyInit__types(void);
extern PyObject *PyInit__typing(void);
extern PyObject *PyInit__abc(void);
extern PyObject *PyInit__locale(void);
extern PyObject *PyInit__opcode(void);

struct _inittab _PyImport_Inittab[] = {
    {"marshal", PyMarshal_Init},
    {"_imp", PyInit__imp},
    {"_ast", PyInit__ast},
    {"_tokenize", PyInit__tokenize},
    {"builtins", NULL},
    {"sys", NULL},
    {"gc", PyInit_gc},
    {"_contextvars", PyInit__contextvars},
    {"_warnings", _PyWarnings_Init},
    {"_string", PyInit__string},
    {"posix", PyInit_posix},
    {"errno", PyInit_errno},
    {"pwd", PyInit_pwd},
    {"grp", PyInit_grp},
    {"_signal", PyInit__signal},
    {"_thread", PyInit__thread},
    {"time", PyInit_time},
    {"_math_integer", PyInit__math_integer},
    {"math", PyInit_math},
    {"mmap", PyInit_mmap},
    {"pyexpat", PyInit_pyexpat},
    {"array", PyInit_array},
    {"_bisect", PyInit__bisect},
    {"_csv", PyInit__csv},
    {"_heapq", PyInit__heapq},
    {"_json", PyInit__json},
    {"_pickle", PyInit__pickle},
    {"_random", PyInit__random},
    {"_struct", PyInit__struct},
    {"binascii", PyInit_binascii},
    {"zlib", PyInit_zlib},
    {"fcntl", PyInit_fcntl},
    {"select", PyInit_select},
    {"_socket", PyInit__socket},
    {"_ssl", PyInit__ssl},
    {"_hashlib", PyInit__hashlib},
    {"unicodedata", PyInit_unicodedata},
    {"_posixsubprocess", PyInit__posixsubprocess},
    {"_sre", PyInit__sre},
    {"_io", PyInit__io},
    {"atexit", PyInit_atexit},
    {"_stat", PyInit__stat},
    {"_weakref", PyInit__weakref},
    {"_codecs", PyInit__codecs},
    {"_collections", PyInit__collections},
    {"itertools", PyInit_itertools},
    {"_functools", PyInit__functools},
    {"_operator", PyInit__operator},
    {"_datetime", PyInit__datetime},
    {"_symtable", PyInit__symtable},
    {"faulthandler", PyInit_faulthandler},
    {"_tracemalloc", PyInit__tracemalloc},
    {"_suggestions", PyInit__suggestions},
    {"_sysconfig", PyInit__sysconfig},
    {"_types", PyInit__types},
    {"_typing", PyInit__typing},
    {"_abc", PyInit__abc},
    {"_locale", PyInit__locale},
    {"_opcode", PyInit__opcode},
    {0, 0}
};
