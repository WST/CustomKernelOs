/* Типы и вывод типов выражений. Размеры — как у gcc для i386: int и указатель по 4 байта. */
#include "cc.h"

static Type t_void   = { TY_VOID, 1, 1 };
static Type t_bool   = { TY_BOOL, 1, 1 };
static Type t_char   = { TY_CHAR, 1, 1 };
static Type t_short  = { TY_SHORT, 2, 2 };
static Type t_int    = { TY_INT, 4, 4 };
static Type t_uchar  = { TY_CHAR, 1, 1, true };
static Type t_ushort = { TY_SHORT, 2, 2, true };
static Type t_uint   = { TY_INT, 4, 4, true };
static Type t_float  = { TY_FLOAT, 4, 4 };
static Type t_double = { TY_DOUBLE, 8, 4 };

Type *ty_void = &t_void, *ty_bool = &t_bool, *ty_char = &t_char, *ty_short = &t_short, *ty_int = &t_int;
Type *ty_uchar = &t_uchar, *ty_ushort = &t_ushort, *ty_uint = &t_uint, *ty_float = &t_float, *ty_double = &t_double;

static Type *new_type(TypeKind kind, int size, int align) {
    Type *ty = xcalloc(sizeof(Type));
    ty->kind = kind;
    ty->size = size;
    ty->align = align;
    return ty;
}

bool is_integer(Type *ty) {
    TypeKind k = ty->kind;
    return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_ENUM;
}
bool is_flonum(Type *ty) { return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE; }
bool is_numeric(Type *ty) { return is_integer(ty) || is_flonum(ty); }

bool is_compatible(Type *a, Type *b) {
    if (a == b) return true;
    if (a->origin) return is_compatible(a->origin, b);
    if (b->origin) return is_compatible(a, b->origin);
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case TY_CHAR: case TY_SHORT: case TY_INT: return a->is_unsigned == b->is_unsigned;
    case TY_FLOAT: case TY_DOUBLE: case TY_VOID: case TY_BOOL: return true;
    case TY_PTR: return is_compatible(a->base, b->base);
    case TY_FUNC:
        if (!is_compatible(a->return_ty, b->return_ty) || a->is_variadic != b->is_variadic) return false;
        {
            Type *p1 = a->params, *p2 = b->params;
            for (; p1 && p2; p1 = p1->next, p2 = p2->next)
                if (!is_compatible(p1, p2)) return false;
            return p1 == NULL && p2 == NULL;
        }
    case TY_ARRAY:
        if (!is_compatible(a->base, b->base)) return false;
        return a->array_len < 0 || b->array_len < 0 || a->array_len == b->array_len;
    default: return false;
    }
}

Type *copy_type(Type *ty) {
    Type *ret = xcalloc(sizeof(Type));
    *ret = *ty;
    ret->origin = ty;
    return ret;
}

Type *pointer_to(Type *base) {
    Type *ty = new_type(TY_PTR, 4, 4);
    ty->base = base;
    ty->is_unsigned = true;
    return ty;
}

Type *func_type(Type *return_ty) {
    Type *ty = new_type(TY_FUNC, 1, 1);
    ty->return_ty = return_ty;
    return ty;
}

Type *array_of(Type *base, int len) {
    Type *ty = new_type(TY_ARRAY, base->size * len, base->align);
    ty->base = base;
    ty->array_len = len;
    return ty;
}

Type *enum_type(void) { return new_type(TY_ENUM, 4, 4); }
Type *struct_type(void) { return new_type(TY_STRUCT, 0, 1); }

int align_to(int n, int align) { return (n + align - 1) / align * align; }

static Type *get_common_type(Type *a, Type *b) {
    if (a->base) return pointer_to(a->base);
    if (a->kind == TY_FUNC) return pointer_to(a);
    if (b->kind == TY_FUNC) return pointer_to(b);
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return ty_double;
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return ty_float;
    if (a->size < 4) a = ty_int;
    if (b->size < 4) b = ty_int;
    if (a->is_unsigned || b->is_unsigned) return ty_uint;
    return ty_int;
}

/* «Обычные арифметические преобразования»: привести оба операнда к общему типу */
static void usual_arith_conv(Node **lhs, Node **rhs) {
    Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
    *lhs = new_cast(*lhs, ty);
    *rhs = new_cast(*rhs, ty);
}

void add_type(Node *node) {
    if (!node || node->ty) return;
    add_type(node->lhs);
    add_type(node->rhs);
    add_type(node->cond);
    add_type(node->then);
    add_type(node->els);
    add_type(node->init);
    add_type(node->inc);
    for (Node *n = node->body; n; n = n->next) add_type(n);
    for (Node *n = node->args; n; n = n->next) add_type(n);

    switch (node->kind) {
    case ND_NUM:
        node->ty = ty_int;
        return;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR:
        usual_arith_conv(&node->lhs, &node->rhs);
        node->ty = node->lhs->ty;
        return;
    case ND_NEG: {
        Type *ty = get_common_type(ty_int, node->lhs->ty);
        node->lhs = new_cast(node->lhs, ty);
        node->ty = ty;
        return;
    }
    case ND_ASSIGN:
        if (node->lhs->ty->kind == TY_ARRAY) error_tok(node->lhs->tok, "not an lvalue");
        if (node->lhs->ty->kind != TY_STRUCT && node->lhs->ty->kind != TY_UNION)
            node->rhs = new_cast(node->rhs, node->lhs->ty);
        node->ty = node->lhs->ty;
        return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        usual_arith_conv(&node->lhs, &node->rhs);
        node->ty = ty_int;
        return;
    case ND_FUNCALL:
        node->ty = node->func_ty->return_ty;
        return;
    case ND_NOT: case ND_LOGOR: case ND_LOGAND:
        node->ty = ty_int;
        return;
    case ND_BITNOT: case ND_SHL: case ND_SHR:
        node->ty = node->lhs->ty->size < 4 ? ty_int : node->lhs->ty;
        if (node->lhs->ty->size < 4) node->lhs = new_cast(node->lhs, ty_int);
        if (is_flonum(node->ty)) error_tok(node->tok, "invalid operand");
        return;
    case ND_VAR:
        node->ty = node->var->ty;
        return;
    case ND_COND:
        if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
            node->ty = ty_void;
        } else if (node->then->ty->kind == TY_STRUCT || node->then->ty->kind == TY_UNION) {
            node->ty = node->then->ty;
        } else if (node->then->ty->base && is_integer(node->els->ty)) {
            node->ty = node->then->ty;
            node->els = new_cast(node->els, node->ty);
        } else if (node->els->ty->base && is_integer(node->then->ty)) {
            node->ty = node->els->ty;
            node->then = new_cast(node->then, node->ty);
        } else {
            usual_arith_conv(&node->then, &node->els);
            node->ty = node->then->ty;
        }
        return;
    case ND_COMMA:
        node->ty = node->rhs->ty;
        return;
    case ND_MEMBER:
        node->ty = node->member->ty;
        return;
    case ND_ADDR: {
        Type *ty = node->lhs->ty;
        node->ty = ty->kind == TY_ARRAY ? pointer_to(ty->base) : pointer_to(ty);
        return;
    }
    case ND_DEREF:
        if (!node->lhs->ty->base) error_tok(node->tok, "invalid pointer dereference");
        if (node->lhs->ty->base->kind == TY_VOID) error_tok(node->tok, "dereferencing a void pointer");
        node->ty = node->lhs->ty->base;
        return;
    case ND_STMT_EXPR:
        if (node->body) {
            Node *stmt = node->body;
            while (stmt->next) stmt = stmt->next;
            if (stmt->kind == ND_EXPR_STMT) { node->ty = stmt->lhs->ty; return; }
        }
        node->ty = ty_void;
        return;
    case ND_SYSCALL:
        node->ty = ty_int;
        return;
    default:
        node->ty = ty_void;
        return;
    }
}
