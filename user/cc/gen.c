/* Генератор кода: дерево -> машинный код x86 (32 бит), плюс «компоновка» и запись ELF.
 *
 * Модель простая — «стековая машина»:
 *   - результат выражения всегда в eax (целые, указатели), а дробный — в st(0) сопроцессора;
 *   - у структур и массивов «значение» — это их адрес в eax;
 *   - для a + b: считаем b, push eax, считаем a, pop ecx, add eax, ecx.
 * Вызовы — как у gcc (cdecl): аргументы в стек справа налево, ответ в eax / st(0).
 *
 * Байты инструкций пишем прямо в буфер: E("0F B6 C0") — это movzx eax, al. */
#include "cc.h"

/* ------------------------------------------------------------ буферы */

void buf_put(Buf *b, const void *p, int n) {
    if (b->len + n > b->cap) {
        int cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n) cap *= 2;
        unsigned char *d = xcalloc(cap);
        if (b->data) memcpy(d, b->data, b->len);
        free(b->data);
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
void buf_byte(Buf *b, int v) { unsigned char c = v; buf_put(b, &c, 1); }
void buf_u32(Buf *b, unsigned v) { unsigned char c[4] = { v, v >> 8, v >> 16, v >> 24 }; buf_put(b, c, 4); }
static void put_u32_at(Buf *b, int pos, unsigned v) {
    b->data[pos] = v; b->data[pos + 1] = v >> 8; b->data[pos + 2] = v >> 16; b->data[pos + 3] = v >> 24;
}

static Buf text;                    /* код */

static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

/* E("8B 45 08") — записать байты, заданные шестнадцатеричной строкой */
static void E(const char *s) {
    while (*s) {
        if (*s == ' ') { s++; continue; }
        buf_byte(&text, hexval(s[0]) * 16 + hexval(s[1]));
        s += 2;
    }
}
static void imm32(unsigned v) { buf_u32(&text, v); }
static bool fits8(int v) { return v >= -128 && v <= 127; }

/* ------------------------------------------------------------ метки и переходы */

static int  *label_pos;             /* label -> смещение в text (-1 — ещё не поставлена) */
static int   label_cap;
typedef struct { int pos, label; } Fixup;
static Fixup *fixups;
static int    n_fixups, cap_fixups;

static void label_here(int l) {
    if (l >= label_cap) {
        int cap = label_cap ? label_cap : 1024;
        while (cap <= l) cap *= 2;
        int *p = xcalloc(sizeof(int) * cap);
        if (label_pos) memcpy(p, label_pos, sizeof(int) * label_cap);
        for (int i = label_cap; i < cap; i++) p[i] = -1;
        free(label_pos);
        label_pos = p;
        label_cap = cap;
    }
    label_pos[l] = text.len;
}

static void add_fixup(int label) {
    if (n_fixups == cap_fixups) {
        cap_fixups = cap_fixups ? cap_fixups * 2 : 256;
        Fixup *f = xcalloc(sizeof(Fixup) * cap_fixups);
        if (fixups) memcpy(f, fixups, sizeof(Fixup) * n_fixups);
        free(fixups);
        fixups = f;
    }
    fixups[n_fixups].pos = text.len;
    fixups[n_fixups].label = label;
    n_fixups++;
    imm32(0);
}

static void jmp(int l) { E("E9"); add_fixup(l); }
static void je(int l)  { E("0F 84"); add_fixup(l); }
static void jne(int l) { E("0F 85"); add_fixup(l); }

static void resolve_fixups(void) {
    for (int i = 0; i < n_fixups; i++) {
        int l = fixups[i].label;
        if (l >= label_cap || label_pos[l] < 0) error("internal error: unresolved label %d", l);
        put_u32_at(&text, fixups[i].pos, label_pos[l] - (fixups[i].pos + 4));
    }
    n_fixups = 0;
}

/* ------------------------------------------------------------ ссылки на символы */

typedef struct Reloc2 {
    int  pos;                       /* где в text */
    char *name;
    int  type;                      /* R_386_* */
} Reloc2;
static Reloc2 *relocs;
static int n_relocs, cap_relocs;

/* Слагаемое лежит в самих байтах. У PC32/PLT32 это −4: процессор считает
 * смещение от следующей команды, а P в формуле ELF — начало поля. У GOTPC
 * это +2: после call ebx указывает на add, а поле начинается двумя байтами позже. */
static void sym_ref(char *name, int type) {
    if (n_relocs == cap_relocs) {
        cap_relocs = cap_relocs ? cap_relocs * 2 : 256;
        Reloc2 *r = xcalloc(sizeof(Reloc2) * cap_relocs);
        if (relocs) memcpy(r, relocs, sizeof(Reloc2) * n_relocs);
        free(relocs);
        relocs = r;
    }
    relocs[n_relocs].pos = text.len;
    relocs[n_relocs].name = name;
    relocs[n_relocs].type = type;
    n_relocs++;
    int addend = 0;
    if (type == R_386_PC32 || type == R_386_PLT32) addend = -4;
    else if (type == R_386_GOTPC) addend = 2;
    imm32(addend);
}

/* ------------------------------------------------------------ простые инструкции */

static int depth;                   /* сколько байт мы сами положили в стек (для проверки) */

static void push_eax(void) { E("50"); depth += 4; }
static void pop_ecx(void)  { E("59"); depth -= 4; }

static void mov_eax_imm(int v) {
    if (v == 0) E("31 C0");
    else { E("B8"); imm32(v); }
}

static void add_esp(int n) {
    if (!n) return;
    if (fits8(n)) { E("83 C4"); buf_byte(&text, n); }
    else { E("81 C4"); imm32(n); }
}
static void sub_esp(int n) {
    if (!n) return;
    if (fits8(n)) { E("83 EC"); buf_byte(&text, n); }
    else { E("81 EC"); imm32(n); }
}

/* lea eax, [ebp + off] */
static void lea_eax_ebp(int off) {
    if (fits8(off)) { E("8D 45"); buf_byte(&text, off); }
    else { E("8D 85"); imm32(off); }
}

static void push_double(void) { sub_esp(8); E("DD 1C 24"); depth += 8; }      /* fstp qword [esp] */
static void pop_double_cmp(void) { add_esp(8); depth -= 8; }

/* копировать ecx байт: [eax] -> [edx]; портит esi, edi */
static void copy_bytes(int size) {
    E("89 C6");                     /* mov esi, eax */
    E("89 D7");                     /* mov edi, edx */
    E("B9"); imm32(size);           /* mov ecx, size */
    E("F3 A4");                     /* rep movsb */
}

/* ------------------------------------------------------------ загрузка и запись */

static void load(Type *ty) {
    switch (ty->kind) {
    case TY_ARRAY: case TY_STRUCT: case TY_UNION: case TY_FUNC: return;
    case TY_FLOAT: E("D9 00"); return;                          /* fld dword [eax] */
    case TY_DOUBLE: E("DD 00"); return;                         /* fld qword [eax] */
    default: break;
    }
    if (ty->kind == TY_LONGLONG) { E("89 C1 8B 01 8B 51 04"); return; } /* edx:eax = [eax] */
    if (ty->size == 1) { if (ty->is_unsigned || ty->kind == TY_BOOL) E("0F B6 00"); else E("0F BE 00"); }
    else if (ty->size == 2) { if (ty->is_unsigned) E("0F B7 00"); else E("0F BF 00"); }
    else E("8B 00");                                            /* mov eax, [eax] */
}

static void load_unit(int sz) {
    if (sz == 1) E("0F B6 00");
    else if (sz == 2) E("0F B7 00");
    else E("8B 00");
}

static void bitfield_extract(Member *mem) {
    int sh = mem->bit_off, w = mem->bit_width;
    load_unit(mem->ty->size);
    if (sh) { E("C1 E8"); buf_byte(&text, sh); }
    if (w >= 32) return;
    E("25"); imm32((int)((1u << w) - 1));
    if (!mem->ty->is_unsigned && w > 0) {
        int n = 32 - w;
        E("C1 E0"); buf_byte(&text, n);
        E("C1 F8"); buf_byte(&text, n);
    }
}

static void gen_expr(Node *node);
static void gen_addr(Node *node);

static void bitfield_assign(Node *node) {
    Member *mem = node->lhs->member;
    int sh = mem->bit_off, w = mem->bit_width, sz = mem->ty->size;
    unsigned mask = w >= 32 ? 0xffffffffu : (1u << w) - 1;
    gen_expr(node->rhs);
    if (w < 32) { E("25"); imm32((int)mask); }
    if (!mem->ty->is_unsigned && w > 0 && w < 32) {
        int n = 32 - w;
        E("C1 E0"); buf_byte(&text, n);
        E("C1 F8"); buf_byte(&text, n);
    }
    push_eax();
    gen_addr(node->lhs);
    E("89 C1");                                                 /* mov ecx, eax */
    if (w >= 32) E("31 C0");
    else {
        load_unit(sz);
        E("25"); imm32((int)~(mask << sh));
    }
    E("5A"); depth -= 4;                                        /* pop edx */
    E("52"); depth += 4;
    if (w < 32) { E("81 E2"); imm32((int)mask); }
    if (sh) { E("C1 E2"); buf_byte(&text, sh); }
    E("09 D0");                                                 /* or eax, edx */
    if (sz == 1) E("88 01");
    else if (sz == 2) E("66 89 01");
    else E("89 01");
    E("58"); depth -= 4;                                        /* результат присваивания */
}

/* адрес лежит в стеке, значение — в eax / st(0) */
static void store(Type *ty) {
    pop_ecx();
    switch (ty->kind) {
    case TY_STRUCT: case TY_UNION:
        E("89 CA");                                             /* mov edx, ecx */
        E("51");                                                /* push ecx */
        copy_bytes(ty->size);
        E("58");                                                /* pop eax: значение = адрес приёмника */
        return;
    case TY_FLOAT: E("D9 11"); return;                          /* fst dword [ecx] */
    case TY_DOUBLE: E("DD 11"); return;                         /* fst qword [ecx] */
    default: break;
    }
    if (ty->kind == TY_LONGLONG) { E("89 01 89 51 04"); return; } /* [ecx] = edx:eax */
    if (ty->size == 1) E("88 01");
    else if (ty->size == 2) E("66 89 01");
    else E("89 01");
}

/* сравнить результат с нулём: после этого ZF=1, если он ноль */
static void cmp_zero(Type *ty) {
    if (is_flonum(ty)) {
        E("D9 EE");                                             /* fldz */
        E("DA E9");                                             /* fucompp */
        E("DF E0");                                             /* fnstsw ax */
        E("9E");                                                /* sahf */
        E("7B 02");                                             /* jnp: порядок есть, ZF уже верен */
        E("09 C0");                                             /* or eax, eax: NaN не равен нулю */
    } else if (ty->kind == TY_LONGLONG) {
        E("09 D0 85 C0");                                       /* or eax, edx; test eax, eax */
    } else {
        E("85 C0");                                             /* test eax, eax */
    }
}

/* ------------------------------------------------------------ приведение типов */

enum { I8, I16, I32, U8, U16, U32, F32, F64 };

static int type_id(Type *ty) {
    switch (ty->kind) {
    case TY_CHAR: return ty->is_unsigned ? U8 : I8;
    case TY_SHORT: return ty->is_unsigned ? U16 : I16;
    case TY_INT: return ty->is_unsigned ? U32 : I32;
    case TY_FLOAT: return F32;
    case TY_DOUBLE: return F64;
    case TY_BOOL: return U8;
    case TY_ENUM: return I32;
    default: return U32;
    }
}

static void round_to_float(void) {
    sub_esp(4); E("D9 1C 24"); E("D9 04 24"); add_esp(4);     /* fstp dword [esp]; fld dword [esp] */
}

/* x87 считает в 80 битах. Каждая операция должна округляться к типу Си,
 * иначе x+2^52-2^52 не отбрасывает дробную часть и floor из musl ломается. */
static void round_to_double(void) {
    sub_esp(8); E("DD 1C 24"); E("DD 04 24"); add_esp(8);     /* fstp qword [esp]; fld qword [esp] */
}

static void float_to_int(int wide) {
    sub_esp(12);
    E("D9 3C 24");                                              /* fnstcw [esp] */
    E("66 8B 04 24");                                           /* mov ax, [esp] */
    E("66 0D 00 0C");                                           /* or ax, 0C00h: округлять к нулю, как в Си */
    E("66 89 44 24 02");                                        /* mov [esp+2], ax */
    E("D9 6C 24 02");                                           /* fldcw [esp+2] */
    E("DF 7C 24 04");                                           /* fistp qword [esp+4] */
    E("D9 2C 24");                                              /* fldcw [esp] */
    E("8B 44 24 04");                                           /* mov eax, [esp+4] */
    if (wide) E("8B 54 24 08");                                 /* mov edx, [esp+8] */
    add_esp(12);
}

static void int_truncate(int to) {
    switch (to) {
    case I8:  E("0F BE C0"); break;
    case U8:  E("0F B6 C0"); break;
    case I16: E("0F BF C0"); break;
    case U16: E("0F B7 C0"); break;
    }
}

static void gen_cast(Type *from, Type *to) {
    if (to->kind == TY_VOID) {
        if (is_flonum(from)) E("DD D8");                        /* fstp st0: выбросить */
        return;
    }
    if (to->kind == TY_BOOL) {
        cmp_zero(from);
        E("0F 95 C0 0F B6 C0");                                 /* setne al; movzx eax, al */
        return;
    }
    int f = type_id(from), t = type_id(to);
    if (f == F32 || f == F64) {
        if (t == F64) return;
        if (t == F32) { round_to_float(); return; }
        float_to_int(to->kind == TY_LONGLONG);
        if (to->kind != TY_LONGLONG) int_truncate(t);
        return;
    }
    if (from->kind == TY_LONGLONG) {
        if (to->kind == TY_LONGLONG) return;
        if (t == F64 || t == F32) {
            sub_esp(8);
            E("89 04 24 89 54 24 04 DF 2C 24");                 /* mov [esp], eax; mov [esp+4], edx; fild qword */
            add_esp(8);
            if (t == F32) round_to_float();
            else round_to_double();
            return;
        }
        if (to->kind == TY_BOOL) { cmp_zero(from); E("0F 95 C0 0F B6 C0"); return; }
        if (to->size < 4) int_truncate(t);
        return;
    }
    if (to->kind == TY_LONGLONG) {
        if (from->is_unsigned || from->kind == TY_PTR || from->kind == TY_BOOL) E("31 D2");
        else E("99");                                           /* cdq */
        return;
    }
    if (t == F32 || t == F64) {
        if (f == U32) { E("6A 00 50 DF 2C 24"); add_esp(8); }   /* push 0; push eax; fild qword [esp] */
        else { E("50 DB 04 24"); add_esp(4); }                  /* push eax; fild dword [esp] */
        if (t == F32) round_to_float();
        return;
    }
    if (to->size < 4 && (to->size < from->size || to->is_unsigned != from->is_unsigned)) int_truncate(t);
}

/* ------------------------------------------------------------ выражения */

static Obj *cur_fn;
static int  ret_label;
static bool fn_got;                 /* в этой функции ebx — указатель на GOT */
static int  save_mask;              /* какие из ebx/esi/edi функция портит: 1, 2, 4 */
static int  save_bytes;             /* сколько байт кадра занимают их сохранения */
static int  got_slot;               /* [ebp+got_slot]: копия ebx с адресом GOT */
static void gen_expr(Node *node);
static void gen_stmt(Node *node);
static void reload_got(void);

/* -fPIC: ebx указывает на GOT. Своё (static, литерал) — lea от ebx,
 * чужое — адрес из ячейки GOT, чтобы потом его мог подставить загрузчик. */
static void gen_addr(Node *node) {
    switch (node->kind) {
    case ND_VAR:
        if (node->var->is_local) lea_eax_ebp(node->var->offset);
        else if (opt_pic && (node->var->is_static || node->var->is_rodata)) {
            E("8D 83"); sym_ref(node->var->name, R_386_GOTOFF);   /* lea eax, [ebx + имя@GOTOFF] */
        } else if (opt_pic) {
            E("8B 83"); sym_ref(node->var->name, R_386_GOT32);    /* mov eax, [ebx + имя@GOT] */
        } else {
            E("B8"); sym_ref(node->var->name, R_386_32);          /* mov eax, адрес */
        }
        return;
    case ND_DEREF:
        gen_expr(node->lhs);
        return;
    case ND_COMMA:
        gen_expr(node->lhs);
        if (is_flonum(node->lhs->ty)) E("DD D8");
        gen_addr(node->rhs);
        return;
    case ND_MEMBER:
        gen_addr(node->lhs);
        if (node->member->offset) {
            if (fits8(node->member->offset)) { E("83 C0"); buf_byte(&text, node->member->offset); }
            else { E("05"); imm32(node->member->offset); }
        }
        return;
    case ND_FUNCALL:
        if (node->ret_buffer) { gen_expr(node); return; }
        break;
    case ND_ASSIGN: case ND_COND: case ND_STMT_EXPR:
        if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) { gen_expr(node); return; }
        break;
    default: break;
    }
    error_tok(node->tok, "not an lvalue");
}

/* значение выражения не нужно (оператор-выражение, левая часть запятой) */
static void gen_discard(Node *node) {
    gen_expr(node);
    if (is_flonum(node->ty)) E("DD D8");
}

static void gen_num(Node *node) {
    if (node->ty->kind == TY_DOUBLE || node->ty->kind == TY_FLOAT) {
        double d = node->fval;
        if (d == 0 && 1 / d > 0) { E("D9 EE"); return; }        /* fldz */
        if (d == 1) { E("D9 E8"); return; }                     /* fld1 */
        if (node->ty->kind == TY_FLOAT) {
            float f = (float)d;
            unsigned u;
            memcpy(&u, &f, 4);
            E("68"); imm32(u);
            E("D9 04 24");                                      /* fld dword [esp] */
            add_esp(4);
            return;
        }
        unsigned u[2];
        memcpy(u, &d, 8);
        E("68"); imm32(u[1]);
        E("68"); imm32(u[0]);
        E("DD 04 24");                                          /* fld qword [esp] */
        add_esp(8);
        return;
    }
    if (node->ty->kind == TY_LONGLONG) {
        mov_eax_imm((int)node->ival);
        if ((unsigned)(node->ival >> 32) == 0) E("31 D2");
        else { E("BA"); imm32((unsigned)(node->ival >> 32)); }
        return;
    }
    mov_eax_imm(node->val);
}

static void gen_funcall(Node *node) {
    int nargs = 0;
    for (Node *a = node->args; a; a = a->next) nargs++;
    Node **args = xcalloc(sizeof(Node *) * (nargs + 1));
    int i = 0;
    for (Node *a = node->args; a; a = a->next) args[i++] = a;

    int pushed = 0;
    for (i = nargs - 1; i >= 0; i--) {                          /* справа налево */
        Node *a = args[i];
        gen_expr(a);
        Type *ty = a->ty;
        if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
            int sz = align_to(ty->size, 4);
            sub_esp(sz);
            E("89 E2");                                         /* mov edx, esp */
            copy_bytes(ty->size);
            pushed += sz;
        } else if (ty->kind == TY_FLOAT) {
            sub_esp(4); E("D9 1C 24");                          /* fstp dword [esp] */
            pushed += 4;
        } else if (ty->kind == TY_DOUBLE) {
            sub_esp(8); E("DD 1C 24");
            pushed += 8;
        } else if (ty->kind == TY_LONGLONG) {
            E("52 50");                                         /* push edx; push eax — младшее слово ниже */
            pushed += 8;
        } else {
            E("50");
            pushed += 4;
        }
    }
    free(args);
    if (node->ret_buffer) {                                     /* куда вернуть структуру */
        lea_eax_ebp(node->ret_buffer->offset);
        E("50");
        pushed += 4;
    }
    Node *fn = node->lhs;
    if (fn->kind == ND_VAR && fn->var->ty->kind == TY_FUNC) {
        E("E8");                                                /* call имя или имя@PLT */
        sym_ref(fn->var->name, opt_pic && !fn->var->is_static ? R_386_PLT32 : R_386_PC32);
    } else {
        gen_expr(fn);
        E("FF D0");                                             /* call eax */
    }
    /* по i386 System V скрытый адрес ответа снимает со стека сама функция (ret 4) */
    add_esp(node->ret_buffer ? pushed - 4 : pushed);
    /* char и short в eax определены только в младших битах: функцию мог собрать gcc */
    Type *rt = node->ty;
    if (rt->kind == TY_BOOL) E("0F B6 C0");
    else if (is_integer(rt) && rt->size == 1) E(rt->is_unsigned ? "0F B6 C0" : "0F BE C0");
    else if (is_integer(rt) && rt->size == 2) E(rt->is_unsigned ? "0F B7 C0" : "0F BF C0");
}

static void gen_syscall(Node *node) {
    static const char *pop[] = { "58", "5B", "59", "5A", "5E", "5F" };   /* eax ebx ecx edx esi edi */
    int n = 0;
    for (Node *a = node->args; a; a = a->next) { gen_expr(a); E("50"); n++; }
    for (int i = n - 1; i >= 0; i--) E(pop[i]);
    E("CD 80");                                                 /* int 0x80 */
    reload_got();                                               /* int 0x80 затирает ebx */
}

static void gen_cmp_double(Node *node) {
    gen_expr(node->rhs);
    push_double();
    gen_expr(node->lhs);
    E("DC 1C 24");                                              /* fcomp qword [esp] */
    E("DF E0 9E");                                              /* fnstsw ax; sahf */
    /* NaN неупорядочен: PF=1. Равенство и порядок только при PF=0. */
    switch (node->kind) {
    case ND_EQ: E("0F 94 C0 0F 9B C1 20 C8"); break;            /* sete al; setnp cl; and al, cl */
    case ND_NE: E("0F 95 C0 0F 9A C1 08 C8"); break;            /* setne al; setp cl; or al, cl */
    case ND_LT: E("0F 92 C0 0F 9B C1 20 C8"); break;            /* setb al; setnp cl; and al, cl */
    default:    E("0F 96 C0 0F 9B C1 20 C8"); break;            /* setbe al; setnp cl; and al, cl */
    }
    E("0F B6 C0");
    pop_double_cmp();
}


/* ------------------------------------------------------------ короткие пути
 * Самые частые случаи — локальная переменная и число — обходятся без push/pop. */

static bool is_scalar_int(Type *ty) {
    return (is_integer(ty) || ty->kind == TY_PTR) && ty->size <= 4;
}

/* локальная переменная целого типа или указатель */
static bool is_local_scalar(Node *n) {
    return n->kind == ND_VAR && n->var->is_local && is_scalar_int(n->ty);
}

/* приведение, которое ничего не меняет в 32-битном регистре */
static Node *strip_nop_cast(Node *n) {
    while (n->kind == ND_CAST && is_scalar_int(n->ty) && is_scalar_int(n->lhs->ty) &&
           n->ty->size == 4 && n->ty->kind != TY_BOOL &&
           (n->lhs->ty->size == 4 || n->lhs->kind == ND_VAR || n->lhs->kind == ND_NUM))
        n = n->lhs;
    return n;
}

/* значение можно сразу положить в ecx */
static bool is_simple(Node *n) {
    n = strip_nop_cast(n);
    return (n->kind == ND_NUM && !is_flonum(n->ty)) || is_local_scalar(n);
}

/* modrm с [ebp + off]: reg — номер регистра (0 eax, 1 ecx) */
static void ebp_operand(int reg, int off) {
    if (fits8(off)) { buf_byte(&text, 0x45 | (reg << 3)); buf_byte(&text, off); }
    else { buf_byte(&text, 0x85 | (reg << 3)); imm32(off); }
}

static void load_local(int reg, Node *n) {
    Type *ty = n->ty;
    int off = n->var->offset;
    if (ty->size == 1) { E((ty->is_unsigned || ty->kind == TY_BOOL) ? "0F B6" : "0F BE"); }
    else if (ty->size == 2) { E(ty->is_unsigned ? "0F B7" : "0F BF"); }
    else E("8B");
    ebp_operand(reg, off);
}

static void gen_to_ecx(Node *n) {
    n = strip_nop_cast(n);
    if (n->kind == ND_NUM) { E("B9"); imm32(n->val); }
    else load_local(1, n);
}

static void gen_branch(Node *node, int label, bool when);

static void push_i64(void) { E("52 50"); depth += 8; }          /* push edx; push eax */
static void pop_i64_rhs(void) { E("5B 59"); depth -= 8; }      /* pop ebx (lo); pop ecx (hi) */

static void call_i64(const char *name) {
    E("51 53 52 50");                                           /* push b_hi, b_lo, a_hi, a_lo */
    E("E8");
    sym_ref((char *)name, opt_pic ? R_386_PLT32 : R_386_PC32);
    add_esp(16);
}

static void shl64(void) {
    int ge = new_label(), done = new_label();
    E("83 E1 3F 83 F9 20");                                     /* and ecx, 63; cmp ecx, 32 */
    E("0F 83"); add_fixup(ge);
    E("0F A5 C2 D3 E0");                                        /* shld edx, eax, cl; shl eax, cl */
    jmp(done);
    label_here(ge);
    E("89 C2 31 C0 83 E9 20 D3 E2");                            /* mov edx, eax; xor eax, eax; sub ecx, 32; shl edx, cl */
    label_here(done);
}

static void shr64(int arith) {
    int ge = new_label(), done = new_label();
    E("83 E1 3F 83 F9 20");
    E("0F 83"); add_fixup(ge);
    E("0F AD D0");                                              /* shrd eax, edx, cl */
    E(arith ? "D3 FA" : "D3 EA");                               /* sar/shr edx, cl */
    jmp(done);
    label_here(ge);
    E("89 D0");                                                 /* mov eax, edx */
    if (arith) E("C1 FA 1F"); else E("31 D2");                  /* sar edx, 31 / xor edx, edx */
    E("83 E9 20");
    E(arith ? "D3 F8" : "D3 E8");                               /* sar/shr eax, cl */
    label_here(done);
}

/* a в edx:eax, b в ecx:ebx. Ответ — снова edx:eax. */
static void gen_i64_bin(Node *node) {
    if (node->kind == ND_SHL || node->kind == ND_SHR) {
        gen_expr(node->rhs);
        push_eax();
        gen_expr(node->lhs);
        pop_ecx();
        if (node->kind == ND_SHL) shl64();
        else shr64(!node->ty->is_unsigned);
        return;
    }
    gen_expr(node->rhs);
    push_i64();
    gen_expr(node->lhs);
    pop_i64_rhs();
    int tru = new_label(), fal = new_label(), done = new_label();
    bool uns = node->lhs->ty->is_unsigned;
    switch (node->kind) {
    case ND_ADD: E("01 D8 11 CA"); return;                      /* add eax, ebx; adc edx, ecx */
    case ND_SUB: E("29 D8 19 CA"); return;                      /* sub eax, ebx; sbb edx, ecx */
    case ND_BITAND: E("21 D8 21 CA"); return;
    case ND_BITOR: E("09 D8 09 CA"); return;
    case ND_BITXOR: E("31 D8 31 CA"); return;
    case ND_MUL:
        E("56 57 50 52");                                       /* push esi edi eax edx (a_lo a_hi) */
        E("8B 44 24 04");                                       /* mov eax, [esp+4] a_lo */
        E("F7 E3");                                             /* mul ebx */
        E("89 C6 89 D7");                                       /* mov esi, eax; mov edi, edx */
        E("8B 04 24");                                          /* mov eax, [esp] a_hi */
        E("0F AF C3");                                          /* imul eax, ebx */
        E("01 C7");                                             /* add edi, eax */
        E("8B 44 24 04");                                       /* mov eax, [esp+4] a_lo */
        E("0F AF C1");                                          /* imul eax, ecx */
        E("01 C7 89 F0 89 FA");                                 /* add edi, eax; mov eax, esi; mov edx, edi */
        E("83 C4 08 5F 5E");                                    /* add esp, 8; pop edi; pop esi */
        return;
    case ND_DIV: case ND_MOD:
        call_i64(uns ? (node->kind == ND_DIV ? "__udivdi3" : "__umoddi3")
                     : (node->kind == ND_DIV ? "__divdi3" : "__moddi3"));
        return;
    case ND_EQ: case ND_NE:
        E("39 CA");                                             /* cmp edx, ecx */
        E("0F 85"); add_fixup(fal);
        E("39 D8");                                             /* cmp eax, ebx */
        E("0F 85"); add_fixup(fal);
        mov_eax_imm(1); jmp(done);
        label_here(fal); mov_eax_imm(0);
        label_here(done);
        if (node->kind == ND_NE) E("83 F0 01");
        E("31 D2");
        return;
    case ND_LT: case ND_LE:
        E("39 CA");                                             /* cmp edx, ecx */
        if (uns) {
            E("0F 82"); add_fixup(tru);                         /* jb tru */
            E("0F 87"); add_fixup(fal);                         /* ja fal */
        } else {
            E("0F 8C"); add_fixup(tru);                         /* jl tru */
            E("0F 8F"); add_fixup(fal);                         /* jg fal */
        }
        E("39 D8");                                             /* cmp eax, ebx */
        E(node->kind == ND_LT ? "0F 82" : "0F 86"); add_fixup(tru); /* jb / jbe */
        label_here(fal); mov_eax_imm(0); jmp(done);
        label_here(tru); mov_eax_imm(1);
        label_here(done); E("31 D2");
        return;
    default: error_tok(node->tok, "invalid expression");
    }
}

static void gen_expr(Node *node) {
    switch (node->kind) {
    case ND_NULL_EXPR: return;
    case ND_NUM: gen_num(node); return;
    case ND_NEG:
        gen_expr(node->lhs);
        if (is_flonum(node->ty)) E("D9 E0");                    /* fchs */
        else if (node->ty->kind == TY_LONGLONG) E("F7 D8 83 D2 00 F7 DA"); /* neg eax; adc edx, 0; neg edx */
        else E("F7 D8");                                        /* neg eax */
        return;
    case ND_VAR:
        if (node->var->is_local && is_scalar_int(node->ty)) { load_local(0, node); return; }
        if (node->var->is_local && node->ty->kind == TY_DOUBLE) { E("DD"); ebp_operand(0, node->var->offset); return; }
        if (node->var->is_local && node->ty->kind == TY_FLOAT) { E("D9"); ebp_operand(0, node->var->offset); return; }
        gen_addr(node);
        load(node->ty);
        return;
    case ND_MEMBER:
        gen_addr(node);
        if (node->member->is_bitfield) bitfield_extract(node->member);
        else load(node->ty);
        return;
    case ND_DEREF:
        gen_expr(node->lhs);
        load(node->ty);
        return;
    case ND_ADDR:
        if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield)
            error_tok(node->tok, "cannot take address of bit-field");
        gen_addr(node->lhs);
        return;
    case ND_ASSIGN:
        if (node->lhs->kind == ND_VAR && node->lhs->var->is_local &&
            (is_scalar_int(node->ty) || is_flonum(node->ty))) {     /* x = ... прямо в [ebp+off] */
            gen_expr(node->rhs);
            Type *ty = node->ty;
            int off = node->lhs->var->offset;
            if (ty->kind == TY_DOUBLE) E("DD"), ebp_operand(2, off);          /* fst qword */
            else if (ty->kind == TY_FLOAT) E("D9"), ebp_operand(2, off);      /* fst dword */
            else if (ty->size == 1) E("88"), ebp_operand(0, off);
            else if (ty->size == 2) E("66 89"), ebp_operand(0, off);
            else E("89"), ebp_operand(0, off);
            return;
        }
        if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
            bitfield_assign(node);
            return;
        }
        gen_addr(node->lhs);
        push_eax();
        gen_expr(node->rhs);
        store(node->ty);
        return;
    case ND_STMT_EXPR:
        for (Node *n = node->body; n; n = n->next) {
            if (!n->next && n->kind == ND_EXPR_STMT) gen_expr(n->lhs);
            else gen_stmt(n);
        }
        return;
    case ND_COMMA:
        gen_discard(node->lhs);
        gen_expr(node->rhs);
        return;
    case ND_CAST:
        gen_expr(node->lhs);
        gen_cast(node->lhs->ty, node->ty);
        return;
    case ND_MEMZERO:
        lea_eax_ebp(node->var->offset);
        E("89 C7");                                             /* mov edi, eax */
        E("B9"); imm32(node->var->ty->size);
        E("31 C0 F3 AA");                                       /* xor eax, eax; rep stosb */
        return;
    case ND_COND: {
        int lelse = new_label(), lend = new_label();
        gen_branch(node->cond, lelse, false);
        gen_expr(node->then);
        jmp(lend);
        label_here(lelse);
        gen_expr(node->els);
        label_here(lend);
        return;
    }
    case ND_NOT:
        gen_expr(node->lhs);
        cmp_zero(node->lhs->ty);
        E("0F 94 C0 0F B6 C0");                                 /* sete al; movzx eax, al */
        return;
    case ND_BITNOT:
        gen_expr(node->lhs);
        E("F7 D0");
        if (node->ty->kind == TY_LONGLONG) E("F7 D2");          /* not edx */
        return;
    case ND_LOGAND: {
        int lfalse = new_label(), lend = new_label();
        gen_expr(node->lhs);
        cmp_zero(node->lhs->ty);
        je(lfalse);
        gen_expr(node->rhs);
        cmp_zero(node->rhs->ty);
        je(lfalse);
        mov_eax_imm(1);
        jmp(lend);
        label_here(lfalse);
        mov_eax_imm(0);
        label_here(lend);
        return;
    }
    case ND_LOGOR: {
        int ltrue = new_label(), lend = new_label();
        gen_expr(node->lhs);
        cmp_zero(node->lhs->ty);
        jne(ltrue);
        gen_expr(node->rhs);
        cmp_zero(node->rhs->ty);
        jne(ltrue);
        mov_eax_imm(0);
        jmp(lend);
        label_here(ltrue);
        mov_eax_imm(1);
        label_here(lend);
        return;
    }
    case ND_FUNCALL: gen_funcall(node); return;
    case ND_SYSCALL: gen_syscall(node); return;
    default: break;
    }

    /* двухместные операции */
    if (node->lhs->ty->kind == TY_LONGLONG) { gen_i64_bin(node); reload_got(); return; }
    if (is_flonum(node->lhs->ty)) {
        if (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) {
            gen_cmp_double(node);
            return;
        }
        gen_expr(node->rhs);
        push_double();
        gen_expr(node->lhs);
        switch (node->kind) {
        case ND_ADD: E("DC 04 24"); break;                      /* fadd qword [esp] */
        case ND_SUB: E("DC 24 24"); break;                      /* fsub */
        case ND_MUL: E("DC 0C 24"); break;                      /* fmul */
        case ND_DIV: E("DC 34 24"); break;                      /* fdiv */
        default: error_tok(node->tok, "invalid expression");
        }
        pop_double_cmp();
        if (node->ty->kind == TY_FLOAT) round_to_float();
        else round_to_double();
        return;
    }

    if (is_simple(node->rhs)) {                                 /* правый операнд простой: без push/pop */
        gen_expr(node->lhs);
        gen_to_ecx(node->rhs);
    } else {
        gen_expr(node->rhs);
        push_eax();
        gen_expr(node->lhs);
        pop_ecx();
    }
    bool u = node->lhs->ty->is_unsigned;
    switch (node->kind) {
    case ND_ADD: E("01 C8"); return;
    case ND_SUB: E("29 C8"); return;
    case ND_MUL: E("0F AF C1"); return;
    case ND_DIV: case ND_MOD:
        if (u) E("31 D2 F7 F1");                                /* xor edx, edx; div ecx */
        else E("99 F7 F9");                                     /* cdq; idiv ecx */
        if (node->kind == ND_MOD) E("89 D0");
        return;
    case ND_BITAND: E("21 C8"); return;
    case ND_BITOR: E("09 C8"); return;
    case ND_BITXOR: E("31 C8"); return;
    case ND_SHL: E("D3 E0"); return;
    case ND_SHR: if (node->ty->is_unsigned) E("D3 E8"); else E("D3 F8"); return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        E("39 C8");                                             /* cmp eax, ecx */
        if (node->kind == ND_EQ) E("0F 94 C0");
        else if (node->kind == ND_NE) E("0F 95 C0");
        else if (node->kind == ND_LT) E(u ? "0F 92 C0" : "0F 9C C0");
        else E(u ? "0F 96 C0" : "0F 9E C0");
        E("0F B6 C0");
        return;
    default: break;
    }
    error_tok(node->tok, "invalid expression");
}

/* Переход на label, если условие истинно (when=true) или ложно (when=false).
 * Сравнения целых сразу дают cmp + jcc, без промежуточного 0/1. */
static void gen_branch(Node *node, int label, bool when) {
    switch (node->kind) {
    case ND_NOT:
        gen_branch(node->lhs, label, !when);
        return;
    case ND_LOGAND:
        if (!when) { gen_branch(node->lhs, label, false); gen_branch(node->rhs, label, false); }
        else { int skip = new_label(); gen_branch(node->lhs, skip, false); gen_branch(node->rhs, label, true); label_here(skip); }
        return;
    case ND_LOGOR:
        if (when) { gen_branch(node->lhs, label, true); gen_branch(node->rhs, label, true); }
        else { int skip = new_label(); gen_branch(node->lhs, skip, true); gen_branch(node->rhs, label, false); label_here(skip); }
        return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        if (is_flonum(node->lhs->ty) || node->lhs->ty->kind == TY_LONGLONG) break;
        if (is_simple(node->rhs)) { gen_expr(node->lhs); gen_to_ecx(node->rhs); }
        else { gen_expr(node->rhs); push_eax(); gen_expr(node->lhs); pop_ecx(); }
        E("39 C8");                                             /* cmp eax, ecx */
        {
            bool u = node->lhs->ty->is_unsigned;
            int cc;                                             /* код условия «истина» */
            switch (node->kind) {
            case ND_EQ: cc = 0x84; break;                       /* je */
            case ND_NE: cc = 0x85; break;                       /* jne */
            case ND_LT: cc = u ? 0x82 : 0x8C; break;            /* jb / jl */
            default:    cc = u ? 0x86 : 0x8E; break;            /* jbe / jle */
            }
            if (!when) cc ^= 1;                                 /* обратное условие — соседний код */
            buf_byte(&text, 0x0F);
            buf_byte(&text, cc);
            add_fixup(label);
        }
        return;
    default: break;
    }
    gen_expr(node);
    cmp_zero(node->ty);
    if (when) jne(label); else je(label);
}

/* ------------------------------------------------------------ операторы */

static void gen_stmt(Node *node) {
    switch (node->kind) {
    case ND_IF: {
        int lelse = new_label(), lend = new_label();
        gen_branch(node->cond, lelse, false);
        gen_stmt(node->then);
        jmp(lend);
        label_here(lelse);
        if (node->els) gen_stmt(node->els);
        label_here(lend);
        return;
    }
    case ND_FOR: {
        int lbegin = new_label();
        if (node->init) gen_stmt(node->init);
        label_here(lbegin);
        if (node->cond) gen_branch(node->cond, node->brk_label, false);
        gen_stmt(node->then);
        label_here(node->cont_label);
        if (node->inc) gen_discard(node->inc);
        jmp(lbegin);
        label_here(node->brk_label);
        return;
    }
    case ND_DO: {
        int lbegin = new_label();
        label_here(lbegin);
        gen_stmt(node->then);
        label_here(node->cont_label);
        gen_branch(node->cond, lbegin, true);
        label_here(node->brk_label);
        return;
    }
    case ND_SWITCH:
        gen_expr(node->cond);
        if (is_flonum(node->cond->ty)) error_tok(node->tok, "switch on a floating value");
        if (node->cond->ty->kind == TY_LONGLONG) {
            for (Node *n = node->case_next; n; n = n->case_next) {
                int skip = new_label();
                E("3D"); imm32((unsigned)n->case_begin);        /* cmp eax, lo */
                jne(skip);
                E("81 FA"); imm32(n->case_begin < 0 ? ~0u : 0); /* cmp edx, hi */
                je(n->case_label);
                label_here(skip);
            }
            if (node->default_case) jmp(node->default_case->case_label);
            jmp(node->brk_label);
            gen_stmt(node->then);
            label_here(node->brk_label);
            return;
        }
        for (Node *n = node->case_next; n; n = n->case_next) {
            if (n->case_begin == n->case_end) {
                E("3D"); imm32(n->case_begin);                  /* cmp eax, imm */
                je(n->case_label);
            } else {
                E("89 C1");                                     /* mov ecx, eax */
                E("81 E9"); imm32(n->case_begin);               /* sub ecx, begin */
                E("81 F9"); imm32(n->case_end - n->case_begin); /* cmp ecx, len */
                E("0F 86"); add_fixup(n->case_label);           /* jbe */
            }
        }
        if (node->default_case) jmp(node->default_case->case_label);
        jmp(node->brk_label);
        gen_stmt(node->then);
        label_here(node->brk_label);
        return;
    case ND_CASE:
        label_here(node->case_label);
        gen_stmt(node->lhs);
        return;
    case ND_BLOCK:
        for (Node *n = node->body; n; n = n->next) gen_stmt(n);
        return;
    case ND_GOTO:
        jmp(node->val);
        return;
    case ND_LABEL:
        label_here(node->val);
        gen_stmt(node->lhs);
        return;
    case ND_RETURN:
        if (node->lhs) {
            gen_expr(node->lhs);
            Type *ty = node->lhs->ty;
            if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
                E("8B 55 08");                                  /* mov edx, [ebp+8]: куда класть */
                copy_bytes(ty->size);
                E("8B 45 08");                                  /* mov eax, [ebp+8] */
            }
        }
        jmp(ret_label);
        return;
    case ND_EXPR_STMT:
        gen_discard(node->lhs);
        return;
    default: break;
    }
    error_tok(node->tok, "invalid statement");
}

/* ------------------------------------------------------------ функции */

static char thunk_name[] = "__x86.get_pc_thunk.bx";
static char got_name[] = "_GLOBAL_OFFSET_TABLE_";
static bool need_thunk;

/* Глобальные данные требуют GOT. Прямой вызов не-static функции тоже: в
 * разделяемой библиотеке он идёт через PLT, а PLT читает ячейку как смещение от ebx. */
static bool needs_got(Node *n) {
    if (!n) return false;
    if (n->kind == ND_VAR && n->var && !n->var->is_local) return true;
    if (n->kind == ND_FUNCALL) {
        for (Node *a = n->args; a; a = a->next)
            if (needs_got(a)) return true;
        if (!(n->lhs && n->lhs->kind == ND_VAR && n->lhs->var && n->lhs->var->ty->kind == TY_FUNC))
            return needs_got(n->lhs);
        return !n->lhs->var->is_static;
    }
    if (needs_got(n->lhs) || needs_got(n->rhs) || needs_got(n->cond) || needs_got(n->then) ||
        needs_got(n->els) || needs_got(n->init) || needs_got(n->inc))
        return true;
    for (Node *s = n->body; s; s = s->next)
        if (needs_got(s)) return true;
    return false;
}

static void reload_got(void) {
    if (!fn_got) return;
    E("8B"); ebp_operand(3, got_slot);                          /* mov ebx, [ebp+got_slot] */
}

static void pic_setup(void) {
    E("E8"); sym_ref(thunk_name, R_386_PC32);                   /* ebx = адрес следующей команды */
    E("81 C3"); sym_ref(got_name, R_386_GOTPC);                 /* add ebx, _GLOBAL_OFFSET_TABLE_ */
    E("89"); ebp_operand(3, got_slot);                          /* mov [ebp+got_slot], ebx */
}

static void note_reg(int bit) { save_mask |= bit; }

/* ebx, esi и edi по System V i386 сохраняет вызываемый. В кадр кладём только те,
 * которые эта функция сама затирает: лишние 4–12 байт на каждый вызов — это стек. */
static void scan_clobber(Node *n) {
    if (!n) return;
    if (n->kind == ND_MEMZERO) note_reg(4);
    if (n->kind == ND_SYSCALL) {
        int k = 0;
        for (Node *a = n->args; a; a = a->next) k++;
        if (k >= 2) note_reg(1);                                /* аргумент попадает в ebx */
        if (k >= 5) note_reg(2);
        if (k >= 6) note_reg(4);
    }
    if (n->kind == ND_RETURN && n->lhs && n->lhs->ty &&
        (n->lhs->ty->kind == TY_STRUCT || n->lhs->ty->kind == TY_UNION)) {
        note_reg(2); note_reg(4);                               /* copy_bytes: esi, edi */
    }
    if (n->kind == ND_ASSIGN && n->ty && n->lhs &&
        !(n->lhs->kind == ND_VAR && n->lhs->var && n->lhs->var->is_local &&
          (is_scalar_int(n->ty) || is_flonum(n->ty))) &&
        !(n->lhs->kind == ND_MEMBER && n->lhs->member && n->lhs->member->is_bitfield) &&
        (n->ty->kind == TY_STRUCT || n->ty->kind == TY_UNION)) {
        note_reg(2); note_reg(4);
    }
    if (n->kind == ND_FUNCALL) {
        for (Node *a = n->args; a; a = a->next)
            if (a->ty && (a->ty->kind == TY_STRUCT || a->ty->kind == TY_UNION)) {
                note_reg(2); note_reg(4);
            }
    }
    if (n->lhs && n->lhs->ty && n->lhs->ty->kind == TY_LONGLONG &&
        (n->kind == ND_ADD || n->kind == ND_SUB || n->kind == ND_MUL || n->kind == ND_DIV ||
         n->kind == ND_MOD || n->kind == ND_BITAND || n->kind == ND_BITOR || n->kind == ND_BITXOR ||
         n->kind == ND_EQ || n->kind == ND_NE || n->kind == ND_LT || n->kind == ND_LE)) {
        note_reg(1);                                            /* pop ebx — младшая половина правого */
        if (n->kind == ND_MUL) { note_reg(2); note_reg(4); }
    }
    scan_clobber(n->lhs);
    scan_clobber(n->rhs);
    scan_clobber(n->cond);
    scan_clobber(n->then);
    scan_clobber(n->els);
    scan_clobber(n->init);
    scan_clobber(n->inc);
    for (Node *s = n->body; s; s = s->next) scan_clobber(s);
    for (Node *a = n->args; a; a = a->next) scan_clobber(a);
}

static void assign_offsets(Obj *fn) {
    int off = 8;
    Type *rt = fn->ty->return_ty;
    if (rt->kind == TY_STRUCT || rt->kind == TY_UNION) off += 4;   /* скрытый адрес для ответа */
    for (Obj *p = fn->params; p; p = p->next) {
        p->offset = off;
        off += align_to(p->ty->size, 4);
    }
    int bottom = save_bytes;                                    /* под ebp — только реально сохранённые регистры */
    if (fn_got) bottom += 4;                                    /* и сохранённый указатель на GOT */
    for (Obj *v = fn->locals; v && v != fn->params; v = v->next) {
        int align = v->ty->align < 4 ? (v->ty->size >= 4 ? 4 : v->ty->align) : v->ty->align;
        if (align > 4) align = 4;
        bottom = align_to(bottom + v->ty->size, align);
        v->offset = -bottom;
    }
    fn->stack_size = align_to(bottom, 4) - save_bytes;
}

/* Кадр: [ebp] — старый ebp, ниже — те из ebx, esi, edi, которые функция портит.
 * В -fPIC следом лежит копия ebx, указывающего на GOT, ещё ниже — локальные переменные. */
static void gen_function(Obj *fn) {
    int nsave;
    cur_fn = fn;
    fn_got = opt_pic && needs_got(fn->body);
    if (fn_got) need_thunk = true;
    save_mask = 0;
    scan_clobber(fn->body);
    if (fn_got) note_reg(1);                                    /* pic_setup пишет в ebx */
    nsave = (save_mask & 1 ? 1 : 0) + (save_mask & 2 ? 1 : 0) + (save_mask & 4 ? 1 : 0);
    save_bytes = nsave * 4;
    got_slot = -(save_bytes + 4);
    assign_offsets(fn);
    fn->section = 1;
    fn->addr = text.len;
    ret_label = new_label();
    E("55 89 E5");                                              /* push ebp; mov ebp, esp */
    if (save_mask & 1) E("53");                                 /* push ebx */
    if (save_mask & 2) E("56");                                 /* push esi */
    if (save_mask & 4) E("57");                                 /* push edi */
    if (fn->stack_size) sub_esp(fn->stack_size);
    if (fn_got) pic_setup();
    depth = 0;
    gen_stmt(fn->body);
    if (!strcmp(fn->name, "main")) mov_eax_imm(0);              /* main без return вернёт 0 */
    label_here(ret_label);
    if (save_bytes) { E("8D 65"); buf_byte(&text, -save_bytes); } /* lea esp, [ebp-saved] */
    else E("89 EC");                                            /* mov esp, ebp */
    if (save_mask & 4) E("5F");                                 /* pop edi */
    if (save_mask & 2) E("5E");                                 /* pop esi */
    if (save_mask & 1) E("5B");                                 /* pop ebx */
    E("5D");                                                    /* pop ebp */
    Type *rt = fn->ty->return_ty;
    if (rt->kind == TY_STRUCT || rt->kind == TY_UNION) E("C2 04 00");   /* ret 4 */
    else E("C3");
    resolve_fixups();
    fn->code_end = text.len;
}

/* ------------------------------------------------------------ объектный файл */

#define HBUCKETS 1024
typedef struct Sym { struct Sym *next; char *name; Obj *obj; int idx; } Sym;
static Sym *symtab[HBUCKETS];
static Sym *undefs[HBUCKETS];       /* имя -> номер неопределённого символа в obj */
static ObjFile *obj;

static unsigned shash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h % HBUCKETS;
}

static Obj *sym_find(const char *name) {
    for (Sym *s = symtab[shash(name)]; s; s = s->next)
        if (!strcmp(s->name, name)) return s->obj;
    return NULL;
}

/* собрать определения всех файлов в одну таблицу имён */
static void build_symtab(Obj *prog) {
    for (Obj *o = prog; o; o = o->next) {
        bool def = o->is_function ? (o->body != NULL) : (o->is_definition || o->is_tentative || o->init_data != NULL);
        if (!def) continue;
        unsigned h = shash(o->name);
        Sym *s = symtab[h];
        while (s && strcmp(s->name, o->name)) s = s->next;
        if (s) {
            Obj *old = s->obj;
            if (o->is_function || old->is_function) error("multiple definition of '%s'", o->name);
            if (o->init_data && old->init_data) error("multiple definition of '%s'", o->name);
            if (o->init_data || o->ty->size > old->ty->size) s->obj = o;   /* int x; + int x = 5; */
            continue;
        }
        s = xcalloc(sizeof(Sym));
        s->name = o->name;
        s->obj = o;
        s->next = symtab[h];
        symtab[h] = s;
    }
}

/* оставить только то, до чего можно дойти от корней. Чего здесь нет — найдёт
 * (или не найдёт и сообщит об ошибке) компоновщик в других объектных файлах. */
static void mark_live(const char *name) {
    Obj *o = sym_find(name);
    if (!o || o->is_live) return;
    o->is_live = true;
    for (int i = 0; i < o->n_refs; i++) mark_live(o->refs[i]);
    for (Reloc *r = o->rel; r; r = r->next) mark_live(r->label);
}

static bool emitted(Obj *o) { return o->is_live && sym_find(o->name) == o; }

static int thunk_sym;

static int undef_sym(char *name) {
    unsigned h = shash(name);
    for (Sym *s = undefs[h]; s; s = s->next)
        if (!strcmp(s->name, name)) return s->idx;
    Sym *s = xcalloc(sizeof(Sym));
    s->name = name;
    s->idx = obj_add_sym(obj, name, BIND_GLOBAL, STY_NOTYPE, SYM_UNDEF, 0, 0);
    s->next = undefs[h];
    undefs[h] = s;
    return s->idx;
}

static int sym_of(char *name) {
    if (need_thunk && !strcmp(name, thunk_name)) return thunk_sym;
    Obj *o = sym_find(name);
    if (o && o->section) return o->sym;
    return undef_sym(name);
}

static void define_sym(Obj *o, int sec, unsigned value, unsigned size) {
    o->sym = obj_add_sym(obj, o->name, o->is_static ? BIND_LOCAL : BIND_GLOBAL,
                         o->is_function ? STY_FUNC : STY_OBJECT, sec, value, size);
}

/* переменная или литерал: своя секция, указатели в инициализаторе — перемещения */
static void emit_data(Obj *o, const char *prefix, int kind, int align) {
    int s = obj_add_section(obj, format("%s.%s", prefix, o->name), kind, align,
                            (unsigned char *)o->init_data, o->ty->size);
    define_sym(o, s, 0, o->ty->size);
}

static void emit_data_relocs(Obj *o) {
    Section *sec = obj->secs[obj->syms[o->sym].sec];
    for (Reloc *r = o->rel; r; r = r->next) {
        unsigned char *p = sec->data + r->offset;
        unsigned v = r->addend;
        p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
        obj_add_reloc(sec, r->offset, R_386_32, sym_of(r->label));
    }
}

ObjFile *codegen(Obj *prog, const char *name, char **roots, int n_roots, bool export_all) {
    memset(symtab, 0, sizeof(symtab));
    memset(undefs, 0, sizeof(undefs));
    text.len = 0;
    n_relocs = 0;
    need_thunk = false;
    thunk_sym = -1;
    obj = obj_new(name);

    build_symtab(prog);
    for (int i = 0; i < n_roots; i++) mark_live(roots[i]);
    if (export_all)
        for (Obj *o = prog; o; o = o->next)
            if (!o->is_static && sym_find(o->name) == o) mark_live(o->name);
    /* Деление long long зовётся из генератора, до разметки живых функций этого не видно. */
    mark_live("__divdi3");
    mark_live("__udivdi3");
    mark_live("__moddi3");
    mark_live("__umoddi3");

    /* код всех нужных функций — подряд в text, потом каждая станет своей секцией */
    for (Obj *o = prog; o; o = o->next)
        if (o->is_function && o->body && emitted(o)) gen_function(o);

    /* секции и символы: сначала все определения, потом перемещения (они ссылаются вперёд) */
    for (Obj *o = prog; o; o = o->next) {
        if (!o->is_function || !o->body || !emitted(o)) continue;
        int s = obj_add_section(obj, format(".text.%s", o->name), SEC_TEXT, 1,
                                text.data + o->addr, o->code_end - o->addr);
        define_sym(o, s, 0, o->code_end - o->addr);
    }
    if (need_thunk) {
        static unsigned char thunk[] = { 0x8B, 0x1C, 0x24, 0xC3 };   /* mov ebx, [esp]; ret */
        int s = obj_add_section(obj, ".text.__x86.get_pc_thunk.bx", SEC_TEXT, 1, thunk, 4);
        thunk_sym = obj_add_sym(obj, thunk_name, BIND_LOCAL, STY_FUNC, s, 0, 4);
    }
    for (Obj *o = prog; o; o = o->next) {
        if (o->is_function || !o->is_rodata || !emitted(o)) continue;
        o->section = 1;
        emit_data(o, ".rodata", SEC_RODATA, o->align < 1 ? 1 : o->align > 4 ? 4 : o->align);
    }
    for (Obj *o = prog; o; o = o->next) {
        if (o->is_function || o->is_rodata || !emitted(o)) continue;
        if (o->ty->size < 0) error("'%s' has incomplete type", o->name);
        int al = o->align < 1 ? 1 : o->align;
        if (o->init_data) {
            o->section = 2;
            emit_data(o, ".data", SEC_DATA, al);
        } else if (o->is_static) {
            o->section = 3;
            emit_data(o, ".bss", SEC_BSS, al);
        } else {                                            /* int x; — общий (COMMON), как у gcc -fcommon */
            o->section = 4;
            define_sym(o, SYM_COMMON, al, o->ty->size);
        }
    }

    int ri = 0;
    for (Obj *o = prog; o; o = o->next) {
        if (!o->is_function || !o->body || !emitted(o)) continue;
        Section *sec = obj->secs[obj->syms[o->sym].sec];
        for (; ri < n_relocs && relocs[ri].pos < (int)o->code_end; ri++)
            obj_add_reloc(sec, relocs[ri].pos - o->addr, relocs[ri].type, sym_of(relocs[ri].name));
    }
    for (Obj *o = prog; o; o = o->next)
        if (!o->is_function && o->section && o->section != 4 && emitted(o)) emit_data_relocs(o);
    return obj;
}
