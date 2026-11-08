#include "etcc/ctype.h"

#include <stdio.h>
#include <string.h>

static EtccCType builtins[] = {
    [CT_VOID] = {CT_VOID, 0, 1, false, false, NULL, 0, NULL, 0, false},
    [CT_BOOL] = {CT_BOOL, 1, 1, true, false, NULL, 0, NULL, 0, false},
    [CT_CHAR] = {CT_CHAR, 1, 1, false, false, NULL, 0, NULL, 0, false},
    [CT_SHORT] = {CT_SHORT, 2, 2, false, false, NULL, 0, NULL, 0, false},
    [CT_INT] = {CT_INT, 4, 4, false, false, NULL, 0, NULL, 0, false},
    [CT_LONG] = {CT_LONG, 4, 4, false, false, NULL, 0, NULL, 0, false},
    [CT_LLONG] = {CT_LLONG, 8, 8, false, false, NULL, 0, NULL, 0, false},
    [CT_FLOAT] = {CT_FLOAT, 4, 4, false, false, NULL, 0, NULL, 0, false},
    [CT_DOUBLE] = {CT_DOUBLE, 8, 8, false, false, NULL, 0, NULL, 0, false},
};

EtccCType *etcc_type_builtin(EtccCTypeKind kind) {
    if (kind < CT_VOID || kind > CT_DOUBLE) return NULL;
    return &builtins[kind];
}

EtccCType *etcc_type_copy(EtccArena *a, const EtccCType *t) {
    EtccCType *n = etcc_arena_alloc(a, sizeof(*n), _Alignof(EtccCType));
    *n = *t;
    return n;
}

EtccCType *etcc_type_ptr(EtccArena *a, EtccCType *base) {
    EtccCType *t = etcc_arena_alloc(a, sizeof(*t), _Alignof(EtccCType));
    *t = (EtccCType){
        .kind = CT_PTR,
        .size = 8,
        .align = 8,
        .is_unsigned = true,
        .base = base,
    };
    return t;
}

EtccCType *etcc_type_array(EtccArena *a, EtccCType *base, uint64_t len) {
    EtccCType *t = etcc_arena_alloc(a, sizeof(*t), _Alignof(EtccCType));
    size_t size = 0;
    if (base && base->kind != CT_VOID && base->kind != CT_FUNC && len)
        size = base->size * (size_t)len;
    *t = (EtccCType){
        .kind = CT_ARRAY,
        .size = size,
        .align = base ? base->align : 1,
        .base = base,
        .array_len = len,
    };
    return t;
}

EtccCType *etcc_type_func(EtccArena *a, EtccCType *result, EtccCType **params,
                          size_t nparams, bool variadic) {
    EtccCType *t = etcc_arena_alloc(a, sizeof(*t), _Alignof(EtccCType));
    EtccCType **copy = NULL;
    if (nparams) {
        copy = etcc_arena_alloc(a, nparams * sizeof(*copy), _Alignof(EtccCType *));
        memcpy(copy, params, nparams * sizeof(*copy));
    }
    *t = (EtccCType){
        .kind = CT_FUNC,
        .size = 0,
        .align = 1,
        .base = result,
        .params = copy,
        .nparams = nparams,
        .variadic = variadic,
    };
    return t;
}

EtccCType *etcc_type_struct(EtccArena *a, EtccCTypeKind kind,
                            const char *tag) {
    EtccCType *t = etcc_arena_alloc(a, sizeof(*t), _Alignof(EtccCType));
    *t = (EtccCType){
        .kind = kind,
        .size = 0,
        .align = 1,
        .tag = tag ? etcc_arena_strdup(a, tag) : NULL,
        .is_complete = false,
        .identity = t,
    };
    return t;
}

EtccField *etcc_field_new(EtccArena *a, const char *name, EtccCType *type) {
    EtccField *f = etcc_arena_alloc(a, sizeof(*f), _Alignof(EtccField));
    memset(f, 0, sizeof(*f));
    f->name = etcc_arena_strdup(a, name);
    f->type = type;
    return f;
}

static size_t align_up(size_t value, size_t align) {
    if (align <= 1) return value;
    return (value + align - 1) & ~(align - 1);
}

bool etcc_type_define(EtccArena *a, EtccCType *t, EtccField *fields) {
    (void)a;
    if (!t || (t->kind != CT_STRUCT && t->kind != CT_UNION)) return false;
    bool is_union = t->kind == CT_UNION;

    size_t size = 0;
    size_t align = 1;
    for (EtccField *f = fields; f; f = f->next) {
        if (!f->type) return false;
        if (f->type->kind == CT_VOID || f->type->kind == CT_FUNC) return false;
        if (f->type->kind == CT_ARRAY && f->type->array_len == 0) return false;
        if ((f->type->kind == CT_STRUCT || f->type->kind == CT_UNION) &&
            !f->type->is_complete)
            return false;

        size_t fa = f->type->align ? f->type->align : 1;
        if (fa > align) align = fa;
        if (is_union) {
            f->offset = 0;
            if (f->type->size > size) size = f->type->size;
        } else {
            size = align_up(size, fa);
            f->offset = size;
            size += f->type->size;
        }
    }

    t->fields = fields;
    t->size = align_up(size, align);
    t->align = align;
    t->is_complete = true;
    return true;
}

EtccField *etcc_type_find_field(const EtccCType *t, const char *name,
                                size_t name_len) {
    if (!t || (t->kind != CT_STRUCT && t->kind != CT_UNION)) return NULL;
    for (EtccField *f = t->fields; f; f = f->next) {
        size_t n = strlen(f->name);
        if (n == name_len && memcmp(f->name, name, n) == 0) return f;
    }
    return NULL;
}

bool etcc_type_equal(const EtccCType *a, const EtccCType *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case CT_PTR:
        return etcc_type_equal(a->base, b->base);
    case CT_ARRAY:
        return a->array_len == b->array_len && etcc_type_equal(a->base, b->base);
    case CT_FUNC:
        if (!etcc_type_equal(a->base, b->base)) return false;
        if (a->nparams != b->nparams || a->variadic != b->variadic) return false;
        for (size_t i = 0; i < a->nparams; i++)
            if (!etcc_type_equal(a->params[i], b->params[i])) return false;
        return true;
    case CT_STRUCT:
    case CT_UNION:
        return a->identity == b->identity;
    default:
        return a->is_unsigned == b->is_unsigned;
    }
}

bool etcc_type_is_integer(const EtccCType *t) {
    if (!t) return false;
    switch (t->kind) {
    case CT_BOOL:
    case CT_CHAR:
    case CT_SHORT:
    case CT_INT:
    case CT_LONG:
    case CT_LLONG:
        return true;
    default:
        return false;
    }
}

bool etcc_type_is_arithmetic(const EtccCType *t) {
    return t && (etcc_type_is_integer(t) || t->kind == CT_FLOAT ||
                 t->kind == CT_DOUBLE);
}

bool etcc_type_is_pointer(const EtccCType *t) {
    return t && t->kind == CT_PTR;
}

bool etcc_type_is_aggregate(const EtccCType *t) {
    return t && (t->kind == CT_STRUCT || t->kind == CT_UNION);
}

bool etcc_type_is_scalar(const EtccCType *t) {
    return t && (etcc_type_is_arithmetic(t) || t->kind == CT_PTR);
}

bool etcc_type_is_signed(const EtccCType *t) {
    return etcc_type_is_integer(t) && !t->is_unsigned;
}

int etcc_type_rank(const EtccCType *t) {
    if (!t) return -1;
    switch (t->kind) {
    case CT_BOOL: return 0;
    case CT_CHAR: return 1;
    case CT_SHORT: return 2;
    case CT_INT: return 3;
    case CT_LONG: return 4;
    case CT_LLONG: return 5;
    case CT_FLOAT: return 6;
    case CT_DOUBLE: return 7;
    default: return 8;
    }
}

EtccCType *etcc_type_decay(const EtccCType *t) {
    /* Decayed types are not allocated; callers allocate when needed. */
    if (!t) return NULL;
    return (EtccCType *)t;
}

EtccCType *etcc_type_common_arithmetic(EtccCType *a, EtccCType *b) {
    if (!a || !b) return NULL;
    if (a->kind == CT_DOUBLE || b->kind == CT_DOUBLE)
        return etcc_type_builtin(CT_DOUBLE);
    if (a->kind == CT_FLOAT || b->kind == CT_FLOAT)
        return etcc_type_builtin(CT_FLOAT);

    if (a->kind == b->kind && a->is_unsigned == b->is_unsigned) return a;

    int ra = etcc_type_rank(a), rb = etcc_type_rank(b);
    bool ua = a->is_unsigned, ub = b->is_unsigned;
    if (ua == ub) return ra >= rb ? a : b;

    EtccCType *u = ua ? a : b;
    EtccCType *s = ua ? b : a;
    int ru = etcc_type_rank(u), rs = etcc_type_rank(s);
    if (ru >= rs) return u;
    if (etcc_type_sizeof(s) > etcc_type_sizeof(u)) return s;

    /* Signed type cannot represent the unsigned range: use its unsigned form. */
    static EtccCType unsigned_versions[CT_LLONG + 1];
    unsigned_versions[CT_CHAR] =
        (EtccCType){.kind = CT_CHAR, .size = 1, .align = 1, .is_unsigned = true};
    unsigned_versions[CT_SHORT] =
        (EtccCType){.kind = CT_SHORT, .size = 2, .align = 2, .is_unsigned = true};
    unsigned_versions[CT_INT] =
        (EtccCType){.kind = CT_INT, .size = 4, .align = 4, .is_unsigned = true};
    unsigned_versions[CT_LONG] =
        (EtccCType){.kind = CT_LONG, .size = 4, .align = 4, .is_unsigned = true};
    unsigned_versions[CT_LLONG] =
        (EtccCType){.kind = CT_LLONG, .size = 8, .align = 8, .is_unsigned = true};
    return &unsigned_versions[s->kind];
}

size_t etcc_type_sizeof(const EtccCType *t) {
    return t ? t->size : 0;
}

size_t etcc_type_alignof(const EtccCType *t) {
    return t ? t->align : 1;
}

const char *etcc_type_format(const EtccCType *t, char *buf, size_t size) {
    if (!t) {
        snprintf(buf, size, "<null-type>");
        return buf;
    }
    switch (t->kind) {
    case CT_VOID: snprintf(buf, size, "void"); break;
    case CT_BOOL: snprintf(buf, size, "_Bool"); break;
    case CT_CHAR: snprintf(buf, size, t->is_unsigned ? "unsigned char" : "char"); break;
    case CT_SHORT: snprintf(buf, size, t->is_unsigned ? "unsigned short" : "short"); break;
    case CT_INT: snprintf(buf, size, t->is_unsigned ? "unsigned int" : "int"); break;
    case CT_LONG: snprintf(buf, size, t->is_unsigned ? "unsigned long" : "long"); break;
    case CT_LLONG: snprintf(buf, size, t->is_unsigned ? "unsigned long long" : "long long"); break;
    case CT_FLOAT: snprintf(buf, size, "float"); break;
    case CT_DOUBLE: snprintf(buf, size, "double"); break;
    case CT_PTR: {
        char inner[256];
        etcc_type_format(t->base, inner, sizeof(inner));
        snprintf(buf, size, "%s *", inner);
        break;
    }
    case CT_ARRAY: {
        char inner[256];
        etcc_type_format(t->base, inner, sizeof(inner));
        snprintf(buf, size, "%s[%llu]", inner,
                 (unsigned long long)t->array_len);
        break;
    }
    case CT_STRUCT:
    case CT_UNION: {
        const char *kw = t->kind == CT_UNION ? "union" : "struct";
        if (t->tag) snprintf(buf, size, "%s %s", kw, t->tag);
        else snprintf(buf, size, "%s <anonymous>", kw);
        break;
    }
    case CT_FUNC: {
        char inner[256];
        etcc_type_format(t->base, inner, sizeof(inner));
        snprintf(buf, size, "%s (", inner);
        size_t used = strlen(buf);
        for (size_t i = 0; i < t->nparams && used + 2 < size; i++) {
            char p[128];
            etcc_type_format(t->params[i], p, sizeof(p));
            snprintf(buf + used, size - used, "%s%s", i ? ", " : "", p);
            used = strlen(buf);
        }
        if (t->variadic && used + 4 < size) {
            snprintf(buf + used, size - used, "%s...", t->nparams ? ", " : "");
            used = strlen(buf);
        }
        if (used + 2 < size) snprintf(buf + used, size - used, ")");
        break;
    }
    default:
        snprintf(buf, size, "<type>");
        break;
    }
    return buf;
}
