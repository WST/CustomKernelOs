/* cc — компилятор Си для MyOS.
 *
 * Путь программы через компилятор:
 *   текст  --lex.c-->  токены  --pp.c-->  токены без #директив и макросов
 *          --parse.c-->  дерево (AST) с типами  --gen.c-->  объектный файл в памяти
 *          --link.c-->  исполняемый файл ELF, который ядро MyOS загружает как любую программу
 *                       (или, с -c, объектный файл .o).
 *
 * Ассемблера нет: компилятор сам кодирует инструкции в байты. Свой компоновщик
 * (link.c) раскладывает код и данные по адресам (код с 0x40000000) и принимает
 * .o — свои и от gcc — и библиотеки .a (/lib/libc.a, /lib/libgfx.a). Если архивов
 * нет, библиотеку cc компилирует из исходников в /lib.
 *
 * Собирается и для MyOS (gcc в WSL), и для Linux (CC_HOST) — так его удобно тестировать. */
#ifndef CC_H
#define CC_H

#ifdef CC_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#define STDERR 2
#define STDOUT 1
#else
#include "myos.h"
#endif

typedef struct Type Type;
typedef struct Token Token;
typedef struct Node Node;
typedef struct Obj Obj;
typedef struct Member Member;

/* ------------------------------------------------------------ общее (main.c) */

char *read_file(const char *path, int *len);       /* NULL — нет файла */
void  error(const char *fmt, ...) __attribute__((noreturn));
void  error_tok(Token *tok, const char *fmt, ...) __attribute__((noreturn));
void  warn_tok(Token *tok, const char *fmt, ...);
char *format(const char *fmt, ...);
char *strndup2(const char *s, int n);
void *xcalloc(int n);

extern char **include_paths;
extern int    n_include_paths;
extern bool   opt_linux;                            /* -linux: ELF для Linux (тесты на хосте) */

/* ------------------------------------------------------------ токены (lex.c) */

typedef enum { TK_IDENT, TK_PUNCT, TK_KEYWORD, TK_STR, TK_NUM, TK_PP_NUM, TK_EOF } TokenKind;

typedef struct File {
    char *name;
    char *contents;
} File;

typedef struct Hideset Hideset;

struct Token {
    TokenKind kind;
    Token *next;
    int    val;                 /* TK_NUM: младшие 32 бита (ещё и метки, #if) */
    unsigned long long ival;    /* TK_NUM: полное целое, в том числе long long */
    double fval;                /* TK_NUM: дробное */
    Type  *ty;                  /* TK_NUM, TK_STR */
    char  *loc;                 /* где в исходнике */
    int    len;
    char  *str;                 /* TK_STR: строка с уже раскрытыми \n, \t... */
    int    str_len;             /* вместе с завершающим нулём */
    File  *file;
    int    line;
    bool   at_bol;              /* первый в строке */
    bool   has_space;           /* перед ним был пробел */
    Hideset *hideset;           /* макросы, которые уже раскрывались в этом токене */
};

Token *tokenize(File *f);
Token *tokenize_file(const char *path);
bool   equal(Token *tok, const char *s);
Token *skip(Token *tok, const char *s);
bool   consume(Token **rest, Token *tok, const char *s);
void   convert_keywords(Token *tok);
void   convert_pp_number(Token *tok);
Token *new_eof(Token *tmpl);
Token *copy_token(Token *t);

/* ------------------------------------------------------------ препроцессор (pp.c) */

Token *preprocess(Token *tok);
void   define_macro(const char *name, const char *body);

/* ------------------------------------------------------------ типы (type.c) */

typedef enum {
    TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONGLONG, TY_FLOAT, TY_DOUBLE,
    TY_ENUM, TY_PTR, TY_FUNC, TY_ARRAY, TY_STRUCT, TY_UNION
} TypeKind;

struct Type {
    TypeKind kind;
    int  size;
    int  align;
    bool is_unsigned;
    Type *origin;               /* для совместимости typedef-копий */

    Type *base;                 /* указатель / массив: на что */
    Token *name;                /* имя (у параметров и объявлений) */
    int  array_len;

    Member *members;            /* struct / union */
    bool is_flexible;           /* последний член — массив без размера */
    bool is_complete;

    Type *return_ty;            /* функция */
    Type *params;
    bool is_variadic;
    bool is_prototyped;
    Type *next;
};

struct Member {
    Member *next;
    Type  *ty;
    Token *tok;
    Token *name;
    int    idx;
    int    offset;
};

extern Type *ty_void, *ty_bool, *ty_char, *ty_short, *ty_int, *ty_llong, *ty_ullong;
extern Type *ty_uchar, *ty_ushort, *ty_uint, *ty_float, *ty_double;

bool  is_integer(Type *ty);
bool  is_flonum(Type *ty);
bool  is_numeric(Type *ty);
bool  is_compatible(Type *a, Type *b);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *func_type(Type *return_ty);
Type *array_of(Type *base, int size);
Type *enum_type(void);
Type *struct_type(void);
void  add_type(Node *node);
int   align_to(int n, int align);

/* ------------------------------------------------------------ дерево (parse.c) */

/* Переменная или функция */
struct Obj {
    Obj  *next;
    char *name;
    Type *ty;
    Token *tok;
    bool  is_local;
    int   align;

    int   offset;               /* локальная: смещение от ebp */

    bool  is_function;
    bool  is_definition;
    bool  is_static;
    bool  is_live;              /* функция кому-то нужна (иначе не генерируем) */
    bool  is_used;

    /* глобальная переменная */
    bool  is_tentative;
    char *init_data;
    struct Reloc *rel;
    bool  is_rodata;            /* строковый литерал */

    /* функция */
    Obj  *params;
    Node *body;
    Obj  *locals;
    Obj  *va_area;
    int   stack_size;
    char **refs;                /* какие функции/переменные упоминает (для отсева) */
    int   n_refs, cap_refs;

    /* для генератора кода */
    int   section;              /* 0 — нет, 1 — код, 2 — данные, 3 — bss, 4 — COMMON */
    unsigned addr;
    unsigned code_end;          /* функция: где в буфере кода кончается */
    int   sym;                  /* номер символа в объектном файле */
};

/* Ссылка из данных глобальной переменной на другую (указатель в инициализаторе) */
typedef struct Reloc {
    struct Reloc *next;
    int   offset;
    char *label;
    int   addend;
} Reloc;

typedef enum {
    ND_NULL_EXPR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_NEG, ND_MOD,
    ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE,
    ND_ASSIGN, ND_COND, ND_COMMA, ND_MEMBER, ND_ADDR, ND_DEREF,
    ND_NOT, ND_BITNOT, ND_LOGAND, ND_LOGOR,
    ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE, ND_BLOCK,
    ND_GOTO, ND_LABEL, ND_FUNCALL, ND_EXPR_STMT, ND_STMT_EXPR,
    ND_VAR, ND_NUM, ND_CAST, ND_MEMZERO, ND_SYSCALL
} NodeKind;

struct Node {
    NodeKind kind;
    Node *next;
    Type *ty;
    Token *tok;

    Node *lhs, *rhs;

    /* if / for / while / do */
    Node *cond, *then, *els, *init, *inc;
    int   brk_label, cont_label;

    /* блок и ({ ... }) */
    Node *body;

    /* член структуры */
    Member *member;

    /* вызов функции */
    Type *func_ty;
    Node *args;
    Obj  *ret_buffer;           /* куда вернуть структуру */

    /* goto и метки */
    char *label;
    char *unique_label;
    Node *goto_next;

    /* switch / case */
    Node *case_next;
    Node *default_case;
    int   case_label;
    int   case_begin, case_end;

    Obj   *var;
    int    val;
    unsigned long long ival;    /* ND_NUM */
    double fval;
};

Obj  *parse(Token *tok);
int   const_expr(Token **rest, Token *tok);
Node *new_cast(Node *expr, Type *ty);
int   new_label(void);

/* ------------------------------------------------------------ код (gen.c) */

#define TEXT_BASE 0x40000000u

typedef struct Buf {
    unsigned char *data;
    int len, cap;
} Buf;

void buf_put(Buf *b, const void *p, int n);
void buf_byte(Buf *b, int v);
void buf_u32(Buf *b, unsigned v);

/* ------------------------------------------------------------ объектные файлы и компоновка (link.c)
 *
 * Объектный файл в памяти — то же, что ELF ET_REL: секции с байтами, символы и
 * перемещения. Генератор делает его из дерева, obj_read — из файла .o (своего или gcc),
 * obj_write пишет его в .o, link_program собирает из нескольких программу ET_EXEC. */

enum { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS };
enum { BIND_LOCAL, BIND_GLOBAL, BIND_WEAK };
enum { STY_NOTYPE, STY_OBJECT, STY_FUNC, STY_SECTION };

/* LinkSym.sec: номер секции в файле или одно из этих */
#define SYM_UNDEF  (-1)
#define SYM_COMMON (-2)
#define SYM_ABS    (-3)

/* типы перемещений i386; слагаемое (addend) лежит в самих байтах, как в ELF REL */
#define R_386_NONE  0
#define R_386_32    1
#define R_386_PC32  2
#define R_386_PLT32 4

typedef struct ObjFile ObjFile;
typedef struct GSym GSym;

typedef struct LinkReloc {
    int offset;
    int type;
    int sym;                    /* номер символа в том же файле */
} LinkReloc;

typedef struct Section {
    char *name;
    int   kind;                 /* SEC_* */
    int   align;
    unsigned char *data;        /* у SEC_BSS — NULL */
    int   size;
    LinkReloc *rels;
    int   n_rels, cap_rels;
    ObjFile *file;
    bool  live;                 /* компоновщик: до неё можно дойти от точки входа */
    unsigned addr;
} Section;

typedef struct LinkSym {
    char *name;
    int   bind, type;
    int   sec;                  /* номер секции или SYM_UNDEF / SYM_COMMON / SYM_ABS */
    unsigned value;             /* смещение в секции; у SYM_COMMON — выравнивание */
    unsigned size;
    GSym *global;               /* компоновщик: общая запись для не-локальных */
} LinkSym;

struct ObjFile {
    char *name;
    Section **secs;
    int n_secs, cap_secs;
    LinkSym *syms;
    int n_syms, cap_syms;
    bool from_lib;          /* часть библиотеки: определения уступают определениям программы */
};

/* Статическая библиотека (.a, формат ar): из неё в программу попадают только те
 * члены, которые определяют ещё не найденные символы. Определения из библиотек
 * уступают определениям из .c и .o программы: так свой malloc заменяет библиотечный. */
typedef struct Archive Archive;

ObjFile *obj_new(const char *name);
int  obj_add_section(ObjFile *f, char *name, int kind, int align, unsigned char *data, int size);
int  obj_add_sym(ObjFile *f, char *name, int bind, int type, int sec, unsigned value, unsigned size);
void obj_add_reloc(Section *s, int offset, int type, int sym);
void obj_write(ObjFile *f, const char *path);       /* ELF ET_REL */
ObjFile *obj_read(const char *path);                /* NULL — нет такого файла */
Archive *archive_read(const char *path);            /* NULL — нет такого файла */
void link_program(ObjFile **files, int n_files, Archive **libs, int n_libs, const char *out_path);
void write_file(const char *path, Buf *b);

/* roots — имена, которые нужны снаружи (точка входа, ссылки из .o);
 * export_all — оставить все не-static определения (cc -c) */
ObjFile *codegen(Obj *prog, const char *name, char **roots, int n_roots, bool export_all);

#endif
