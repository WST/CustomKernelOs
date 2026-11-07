/* cc — компилятор Си для MyOS.
 *
 * Путь программы через компилятор:
 *   текст  --lex.c-->  токены  --pp.c-->  токены без #директив и макросов
 *          --parse.c-->  дерево (AST) с типами  --gen.c-->  машинный код x86
 *          --elf.c-->  исполняемый файл ELF, который ядро MyOS загружает как любую программу.
 *
 * Ассемблера и компоновщика нет: компилятор сам кодирует инструкции в байты
 * и сам раскладывает код и данные по адресам (код с 0x40000000). Библиотеку
 * (libc, gfx) он компилирует вместе с программой из исходников в /lib.
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
    int   section;              /* 0 — нет, 1 — код, 2 — данные, 3 — bss */
    unsigned addr;
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

/* ------------------------------------------------------------ код (gen.c, elf.c) */

#define TEXT_BASE 0x40000000u

typedef struct Buf {
    unsigned char *data;
    int len, cap;
} Buf;

void buf_put(Buf *b, const void *p, int n);
void buf_byte(Buf *b, int v);
void buf_u32(Buf *b, unsigned v);

void codegen(Obj *prog, const char *out_path);

#endif
