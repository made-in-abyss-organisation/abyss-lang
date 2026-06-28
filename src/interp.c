#include "interp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- environment (lexical scope) ---------- */

typedef struct Env Env;
struct Env {
    char **names;
    Value *values;
    int count, cap;
    Env *parent;
};

static Env *g_global;  /* top scope; functions close over it */

static Env *env_new(Env *parent) {
    Env *e = (Env *)calloc(1, sizeof(Env));
    e->parent = parent;
    return e;
}

static void env_define(Env *e, const char *name, Value v) {
    if (e->count + 1 > e->cap) {
        e->cap = e->cap < 8 ? 8 : e->cap * 2;
        e->names = (char **)realloc(e->names, sizeof(char *) * e->cap);
        e->values = (Value *)realloc(e->values, sizeof(Value) * e->cap);
    }
    e->names[e->count] = (char *)name;
    e->values[e->count] = v;
    e->count++;
}

static int env_get(Env *e, const char *name, Value *out) {
    for (; e; e = e->parent) {
        for (int i = e->count - 1; i >= 0; i--) {  /* backward = inner shadows */
            if (strcmp(e->names[i], name) == 0) { *out = e->values[i]; return 1; }
        }
    }
    return 0;
}

static int env_assign(Env *e, const char *name, Value v) {
    for (; e; e = e->parent) {
        for (int i = e->count - 1; i >= 0; i--) {
            if (strcmp(e->names[i], name) == 0) { e->values[i] = v; return 1; }
        }
    }
    return 0;
}

/* ---------- value helpers ---------- */

static Value v_nil(void)            { Value v; v.type = VAL_NIL; return v; }
static Value v_bool(int b)          { Value v; v.type = VAL_BOOL; v.as.boolean = b; return v; }
static Value v_int(long long i)     { Value v; v.type = VAL_INT; v.as.integer = i; return v; }
static Value v_float(double f)      { Value v; v.type = VAL_FLOAT; v.as.floating = f; return v; }
static Value v_string(char *s)      { Value v; v.type = VAL_STRING; v.as.string = s; return v; }
static Value v_fn(Node *decl)       { Value v; v.type = VAL_FN; v.as.fn.decl = decl; return v; }
static Value v_native(NativeFn fn)  { Value v; v.type = VAL_NATIVE; v.as.native = fn; return v; }

static int is_num(Value v)   { return v.type == VAL_INT || v.type == VAL_FLOAT; }
static double as_num(Value v){ return v.type == VAL_INT ? (double)v.as.integer : v.as.floating; }

static int is_truthy(Value v) {
    switch (v.type) {
        case VAL_NIL:   return 0;
        case VAL_BOOL:  return v.as.boolean;
        case VAL_INT:   return v.as.integer != 0;
        case VAL_FLOAT: return v.as.floating != 0.0;
        default:        return 1;
    }
}

static void runtime_error(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "abyss runtime error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(70);
}

static void print_value(Value v) {
    switch (v.type) {
        case VAL_NIL:    printf("nil"); break;
        case VAL_BOOL:   printf(v.as.boolean ? "true" : "false"); break;
        case VAL_INT:    printf("%lld", v.as.integer); break;
        case VAL_FLOAT:  printf("%g", v.as.floating); break;
        case VAL_STRING: printf("%s", v.as.string); break;
        case VAL_FN:     printf("<fn %s>", v.as.fn.decl->as.fn_decl.name); break;
        case VAL_NATIVE: printf("<native fn>"); break;
    }
}

/* ---------- built-in functions ---------- */

static Value native_print(int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) printf(" ");
        print_value(argv[i]);
    }
    printf("\n");
    return v_nil();
}

/* ---------- evaluation ---------- */

typedef enum { EXEC_NORMAL, EXEC_RETURN } ExecStatus;

static Value eval(Node *n, Env *env);
static ExecStatus exec(Node *n, Env *env, Value *ret);

static char *copy_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    memcpy(p, s, n);
    return p;
}

static char *value_to_cstr(Value v) {
    char buf[64];
    switch (v.type) {
        case VAL_STRING: return copy_cstr(v.as.string);
        case VAL_INT:    snprintf(buf, sizeof buf, "%lld", v.as.integer); return copy_cstr(buf);
        case VAL_FLOAT:  snprintf(buf, sizeof buf, "%g", v.as.floating);  return copy_cstr(buf);
        case VAL_BOOL:   return copy_cstr(v.as.boolean ? "true" : "false");
        case VAL_NIL:    return copy_cstr("nil");
        default:         return copy_cstr("<fn>");
    }
}

/* Evaluate a string literal, expanding ${name} interpolations. */
static Value eval_string(Node *n, Env *env) {
    const char *t = n->as.literal.text;   /* includes surrounding quotes */
    int len = (int)strlen(t);
    int cap = 32, used = 0;
    char *out = (char *)malloc(cap);
    for (int i = 1; i < len - 1; ) {
        if (t[i] == '$' && i + 1 < len - 1 && t[i + 1] == '{') {
            int j = i + 2;
            while (j < len - 1 && t[j] != '}') j++;
            int s = i + 2, e = j;
            while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
            while (e > s && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
            char name[128];
            int nl = e - s; if (nl > 127) nl = 127;
            memcpy(name, t + s, (size_t)nl); name[nl] = '\0';
            Value v;
            if (!env_get(env, name, &v))
                runtime_error("undefined variable '%s' in string", name);
            char *vs = value_to_cstr(v);
            for (char *p = vs; *p; p++) {
                if (used + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                out[used++] = *p;
            }
            free(vs);
            i = (j < len - 1) ? j + 1 : j;
        } else {
            if (used + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
            out[used++] = t[i++];
        }
    }
    if (used + 1 > cap) { out = (char *)realloc(out, used + 1); }
    out[used] = '\0';
    return v_string(out);
}

static int values_equal(Value a, Value b) {
    if (is_num(a) && is_num(b)) return as_num(a) == as_num(b);
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_NIL:    return 1;
        case VAL_BOOL:   return a.as.boolean == b.as.boolean;
        case VAL_STRING: return strcmp(a.as.string, b.as.string) == 0;
        default:         return 0;
    }
}

static Value apply_binary(const char *op, Value l, Value r) {
    /* string concatenation with + */
    if (op[0] == '+' && op[1] == '\0' &&
        l.type == VAL_STRING && r.type == VAL_STRING) {
        size_t n = strlen(l.as.string) + strlen(r.as.string) + 1;
        char *s = (char *)malloc(n);
        snprintf(s, n, "%s%s", l.as.string, r.as.string);
        return v_string(s);
    }
    if (strcmp(op, "==") == 0) return v_bool(values_equal(l, r));
    if (strcmp(op, "!=") == 0) return v_bool(!values_equal(l, r));

    if (!is_num(l) || !is_num(r))
        runtime_error("operator '%s' needs numbers", op);

    /* integer arithmetic stays integer; any float promotes to float */
    int both_int = (l.type == VAL_INT && r.type == VAL_INT);
    if (both_int) {
        long long a = l.as.integer, b = r.as.integer;
        if (op[0] == '+' && !op[1]) return v_int(a + b);
        if (op[0] == '-' && !op[1]) return v_int(a - b);
        if (op[0] == '*' && !op[1]) return v_int(a * b);
        if (op[0] == '/' && !op[1]) { if (!b) runtime_error("division by zero"); return v_int(a / b); }
        if (op[0] == '%' && !op[1]) { if (!b) runtime_error("modulo by zero"); return v_int(a % b); }
    } else {
        double a = as_num(l), b = as_num(r);
        if (op[0] == '+' && !op[1]) return v_float(a + b);
        if (op[0] == '-' && !op[1]) return v_float(a - b);
        if (op[0] == '*' && !op[1]) return v_float(a * b);
        if (op[0] == '/' && !op[1]) return v_float(a / b);
    }

    double a = as_num(l), b = as_num(r);
    if (strcmp(op, "<") == 0)  return v_bool(a < b);
    if (strcmp(op, ">") == 0)  return v_bool(a > b);
    if (strcmp(op, "<=") == 0) return v_bool(a <= b);
    if (strcmp(op, ">=") == 0) return v_bool(a >= b);

    runtime_error("unknown operator '%s'", op);
    return v_nil();
}

static Value call_value(Value callee, Value *argv, int argc) {
    if (callee.type == VAL_NATIVE) return callee.as.native(argc, argv);
    if (callee.type == VAL_FN) {
        Node *decl = callee.as.fn.decl;
        NodeList *params = &decl->as.fn_decl.params;
        if (argc != params->count)
            runtime_error("'%s' expects %d argument(s), got %d",
                          decl->as.fn_decl.name, params->count, argc);
        Env *local = env_new(g_global);
        for (int i = 0; i < argc; i++)
            env_define(local, params->items[i]->as.param.name, argv[i]);
        Value ret = v_nil();
        exec(decl->as.fn_decl.body, local, &ret);
        return ret;
    }
    runtime_error("value is not callable");
    return v_nil();
}

static Value eval_literal(Node *n) {
    char *t = n->as.literal.text;
    switch (n->as.literal.kind) {
        case TOK_INT:   return v_int(strtoll(t, NULL, 10));
        case TOK_FLOAT: return v_float(strtod(t, NULL));
        case TOK_TRUE:  return v_bool(1);
        case TOK_FALSE: return v_bool(0);
        case TOK_NIL:   return v_nil();
        case TOK_STRING: {
            int len = (int)strlen(t);             /* includes surrounding quotes */
            int inner = len >= 2 ? len - 2 : 0;
            char *s = (char *)malloc((size_t)inner + 1);
            memcpy(s, t + 1, (size_t)inner);
            s[inner] = '\0';
            return v_string(s);
        }
        default: return v_nil();
    }
}

static Value eval(Node *n, Env *env) {
    switch (n->type) {
        case NODE_LITERAL:
            if (n->as.literal.kind == TOK_STRING) return eval_string(n, env);
            return eval_literal(n);
        case NODE_IDENT: {
            Value v;
            if (!env_get(env, n->as.ident.name, &v))
                runtime_error("undefined variable '%s'", n->as.ident.name);
            return v;
        }
        case NODE_UNARY: {
            Value operand = eval(n->as.unary.operand, env);
            if (n->as.unary.op[0] == '-') {
                if (operand.type == VAL_INT)   return v_int(-operand.as.integer);
                if (operand.type == VAL_FLOAT) return v_float(-operand.as.floating);
                runtime_error("unary '-' needs a number");
            }
            if (n->as.unary.op[0] == '!') return v_bool(!is_truthy(operand));
            return v_nil();
        }
        case NODE_BINARY: {
            const char *op = n->as.binary.op;
            if (strcmp(op, "&&") == 0) {
                Value l = eval(n->as.binary.left, env);
                if (!is_truthy(l)) return v_bool(0);
                return v_bool(is_truthy(eval(n->as.binary.right, env)));
            }
            if (strcmp(op, "||") == 0) {
                Value l = eval(n->as.binary.left, env);
                if (is_truthy(l)) return v_bool(1);
                return v_bool(is_truthy(eval(n->as.binary.right, env)));
            }
            if (strcmp(op, "??") == 0) {
                Value l = eval(n->as.binary.left, env);
                return l.type == VAL_NIL ? eval(n->as.binary.right, env) : l;
            }
            Value l = eval(n->as.binary.left, env);
            Value r = eval(n->as.binary.right, env);
            return apply_binary(op, l, r);
        }
        case NODE_ASSIGN: {
            if (n->as.assign.target->type != NODE_IDENT)
                runtime_error("can only assign to a variable");
            const char *name = n->as.assign.target->as.ident.name;
            Value value = eval(n->as.assign.value, env);
            const char *op = n->as.assign.op;
            if (op[0] != '=') {  /* += or -= */
                Value cur;
                if (!env_get(env, name, &cur))
                    runtime_error("undefined variable '%s'", name);
                char binop[2] = { op[0], '\0' };
                value = apply_binary(binop, cur, value);
            }
            if (!env_assign(env, name, value))
                runtime_error("undefined variable '%s'", name);
            return value;
        }
        case NODE_CALL: {
            Value callee = eval(n->as.call.callee, env);
            int argc = n->as.call.args.count;
            if (argc > 64) runtime_error("too many arguments");
            Value argv[64];
            for (int i = 0; i < argc; i++)
                argv[i] = eval(n->as.call.args.items[i], env);
            return call_value(callee, argv, argc);
        }
        case NODE_GET: {
            Value obj = eval(n->as.get.object, env);
            if (n->as.get.safe && obj.type == VAL_NIL) return v_nil();
            runtime_error("field access is not supported yet (no structs at runtime)");
            return v_nil();
        }
        default:
            runtime_error("cannot evaluate this expression");
            return v_nil();
    }
}

static ExecStatus exec(Node *n, Env *env, Value *ret) {
    switch (n->type) {
        case NODE_BLOCK: {
            Env *scope = env_new(env);
            for (int i = 0; i < n->as.block.statements.count; i++) {
                if (exec(n->as.block.statements.items[i], scope, ret) == EXEC_RETURN)
                    return EXEC_RETURN;
            }
            return EXEC_NORMAL;
        }
        case NODE_VAR_DECL: {
            Value v = n->as.var_decl.init ? eval(n->as.var_decl.init, env) : v_nil();
            env_define(env, n->as.var_decl.name, v);
            return EXEC_NORMAL;
        }
        case NODE_FN_DECL:
            env_define(env, n->as.fn_decl.name, v_fn(n));
            return EXEC_NORMAL;
        case NODE_RETURN:
            *ret = n->as.ret.value ? eval(n->as.ret.value, env) : v_nil();
            return EXEC_RETURN;
        case NODE_IF:
            if (is_truthy(eval(n->as.if_stmt.cond, env)))
                return exec(n->as.if_stmt.then_branch, env, ret);
            if (n->as.if_stmt.else_branch)
                return exec(n->as.if_stmt.else_branch, env, ret);
            return EXEC_NORMAL;
        case NODE_EXPR_STMT:
            eval(n->as.expr_stmt.expr, env);
            return EXEC_NORMAL;
        default:
            /* imports, components, etc. have no runtime effect yet */
            return EXEC_NORMAL;
    }
}

/* ---------- entry point ---------- */

int interpret(Node *program) {
    g_global = env_new(NULL);
    env_define(g_global, "print", v_native(native_print));

    NodeList *decls = &program->as.program.declarations;
    for (int i = 0; i < decls->count; i++) {
        Node *d = decls->items[i];
        if (d->type == NODE_FN_DECL) {
            env_define(g_global, d->as.fn_decl.name, v_fn(d));
        } else if (d->type == NODE_VAR_DECL) {
            Value v = d->as.var_decl.init ? eval(d->as.var_decl.init, g_global) : v_nil();
            env_define(g_global, d->as.var_decl.name, v);
        }
        /* imports & components are inert until later phases */
    }

    Value main_fn;
    if (!env_get(g_global, "main", &main_fn) || main_fn.type != VAL_FN) {
        fprintf(stderr, "abyss: no 'main' function to run\n");
        return 0;
    }
    call_value(main_fn, NULL, 0);
    return 0;
}
