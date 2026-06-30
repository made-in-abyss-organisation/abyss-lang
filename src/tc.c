#include "tc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- types ---------- */

typedef enum {
    TY_NIL, TY_INT, TY_FLOAT, TY_BOOL, TY_STRING, TY_VOID,
    TY_RANGE, TY_LIST, TY_STRUCT, TY_STRUCT_DEF, TY_FN,
    TY_COMPONENT_DEF, TY_ANY, TY_ERROR
} TypeKind;

typedef struct {
    TypeKind kind;
    Node *decl;   /* NODE_STRUCT for STRUCT/STRUCT_DEF, NODE_FN_DECL for FN */
} Type;

static Type ty(TypeKind k)            { Type t; t.kind = k; t.decl = NULL; return t; }
static Type ty_struct(Node *d)        { Type t; t.kind = TY_STRUCT; t.decl = d; return t; }
static Type ty_struct_def(Node *d)    { Type t; t.kind = TY_STRUCT_DEF; t.decl = d; return t; }
static Type ty_fn(Node *d)            { Type t; t.kind = TY_FN; t.decl = d; return t; }
static Type ty_component_def(Node *d) { Type t; t.kind = TY_COMPONENT_DEF; t.decl = d; return t; }

static int is_num(TypeKind k)     { return k == TY_INT || k == TY_FLOAT; }
static int is_unknown(TypeKind k) { return k == TY_ANY || k == TY_ERROR; }

/* ---------- typing environment ---------- */

typedef struct TypeEnv TypeEnv;
struct TypeEnv {
    char **names;
    Type *types;
    int count, cap;
    TypeEnv *parent;
};

static TypeEnv *te_new(TypeEnv *parent) {
    TypeEnv *e = (TypeEnv *)calloc(1, sizeof(TypeEnv));
    e->parent = parent;
    return e;
}

static void te_define(TypeEnv *e, const char *name, Type t) {
    if (e->count + 1 > e->cap) {
        e->cap = e->cap < 8 ? 8 : e->cap * 2;
        e->names = (char **)realloc(e->names, sizeof(char *) * e->cap);
        e->types = (Type *)realloc(e->types, sizeof(Type) * e->cap);
    }
    e->names[e->count] = (char *)name;
    e->types[e->count] = t;
    e->count++;
}

static int te_get(TypeEnv *e, const char *name, Type *out) {
    for (; e; e = e->parent)
        for (int i = e->count - 1; i >= 0; i--)
            if (strcmp(e->names[i], name) == 0) { *out = e->types[i]; return 1; }
    return 0;
}

/* ---------- checker state ---------- */

static int g_errors;
static TypeEnv *g_globals;
static Type g_return;   /* expected return type of the function being checked */

static void terror(int line, const char *fmt, ...) {
    va_list ap;
    g_errors++;
    fprintf(stderr, "[line %d] type error: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static const char *kind_name(TypeKind k) {
    switch (k) {
        case TY_NIL: return "Nil"; case TY_INT: return "Int";
        case TY_FLOAT: return "Float"; case TY_BOOL: return "Bool";
        case TY_STRING: return "String"; case TY_VOID: return "Void";
        case TY_RANGE: return "Range"; case TY_LIST: return "List";
        case TY_STRUCT: return "struct";
        case TY_STRUCT_DEF: return "type"; case TY_FN: return "function";
        case TY_COMPONENT_DEF: return "component";
        default: return "?";
    }
}

/* Resolve a written type name ("Int", "Point", "List<Int>?") to a Type.
 * Unknown / generic / nullable types degrade to ANY (no false positives). */
static Type type_from_name(const char *name) {
    if (!name) return ty(TY_ANY);
    int n = 0;                              /* length of the base name */
    while (name[n] && name[n] != '?' && name[n] != '<') n++;
    if (n == 3 && !strncmp(name, "Int", 3))    return ty(TY_INT);
    if (n == 5 && !strncmp(name, "Float", 5))  return ty(TY_FLOAT);
    if (n == 4 && !strncmp(name, "Bool", 4))   return ty(TY_BOOL);
    if (n == 6 && !strncmp(name, "String", 6)) return ty(TY_STRING);
    if (n == 4 && !strncmp(name, "List", 4))   return ty(TY_LIST);
    Type t;
    char saved = name[n];
    ((char *)name)[n] = '\0';
    int found = te_get(g_globals, name, &t);
    ((char *)name)[n] = saved;
    if (found && t.kind == TY_STRUCT_DEF) return ty_struct(t.decl);
    return ty(TY_ANY);
}

/* map a checker type to the codegen tag the backend reads off each node */
static int aty_of(TypeKind k) {
    switch (k) {
        case TY_INT:    return CG_INT;
        case TY_FLOAT:  return CG_FLOAT;
        case TY_BOOL:   return CG_BOOL;
        case TY_STRING: return CG_STR;
        default:        return CG_OTHER;
    }
}

/* expected <- actual: is `actual` usable where `expected` is wanted? */
static int compatible(Type expected, Type actual) {
    if (is_unknown(expected.kind) || is_unknown(actual.kind)) return 1;
    if (expected.kind == TY_NIL || actual.kind == TY_NIL) return 1;
    if (is_num(expected.kind) && is_num(actual.kind)) return 1;
    if (expected.kind == TY_STRUCT && actual.kind == TY_STRUCT)
        return expected.decl == actual.decl;
    return expected.kind == actual.kind;
}

static const char *struct_name(Type t) {
    return (t.kind == TY_STRUCT || t.kind == TY_STRUCT_DEF)
         ? t.decl->as.struct_decl.name : kind_name(t.kind);
}

/* ---------- expression & statement checking ---------- */

static Type check_expr(Node *n, TypeEnv *env);
static Type check_expr_inner(Node *n, TypeEnv *env);
static void check_stmt(Node *n, TypeEnv *env);

static Type check_call(Node *n, TypeEnv *env) {
    Type callee = check_expr(n->as.call.callee, env);
    int argc = n->as.call.args.count;
    Type argv[64];
    for (int i = 0; i < argc && i < 64; i++)
        argv[i] = check_expr(n->as.call.args.items[i], env);

    if (callee.kind == TY_FN) {
        if (callee.decl == NULL) {   /* builtin: variadic, typed by name */
            if (n->as.call.callee->type == NODE_IDENT) {
                const char *bn = n->as.call.callee->as.ident.name;
                if (!strcmp(bn, "len")) {
                    if (argc != 1) terror(n->line, "'len' expects 1 argument, got %d", argc);
                    else if (!is_unknown(argv[0].kind) &&
                             argv[0].kind != TY_LIST && argv[0].kind != TY_STRING)
                        terror(n->line, "'len' expects a List or String, got %s",
                               kind_name(argv[0].kind));
                    return ty(TY_INT);
                }
                if (!strcmp(bn, "push")) {
                    if (argc != 2) terror(n->line, "'push' expects 2 arguments, got %d", argc);
                    else if (!is_unknown(argv[0].kind) && argv[0].kind != TY_LIST)
                        terror(n->line, "'push' expects a List as its first argument, got %s",
                               kind_name(argv[0].kind));
                    return ty(TY_NIL);
                }
            }
            return ty(TY_NIL);   /* print, etc. */
        }
        NodeList *ps = &callee.decl->as.fn_decl.params;
        const char *fname = callee.decl->as.fn_decl.name;
        if (argc != ps->count) {
            terror(n->line, "'%s' expects %d argument(s), got %d", fname, ps->count, argc);
        } else {
            for (int i = 0; i < argc && i < 64; i++) {
                Type pt = type_from_name(ps->items[i]->as.param.param_type);
                if (!compatible(pt, argv[i]))
                    terror(n->line, "argument %d of '%s' expects %s, got %s",
                           i + 1, fname, kind_name(pt.kind), kind_name(argv[i].kind));
            }
        }
        return type_from_name(callee.decl->as.fn_decl.ret_type);
    }
    if (callee.kind == TY_STRUCT_DEF) {
        NodeList *fs = &callee.decl->as.struct_decl.fields;
        const char *sname = callee.decl->as.struct_decl.name;
        if (argc != fs->count) {
            terror(n->line, "struct '%s' expects %d field(s), got %d", sname, fs->count, argc);
        } else {
            for (int i = 0; i < argc && i < 64; i++) {
                Type ft = type_from_name(fs->items[i]->as.param.param_type);
                if (!compatible(ft, argv[i]))
                    terror(n->line, "field '%s' of '%s' expects %s, got %s",
                           fs->items[i]->as.param.name, sname,
                           kind_name(ft.kind), kind_name(argv[i].kind));
            }
        }
        return ty_struct(callee.decl);
    }
    if (callee.kind == TY_COMPONENT_DEF)   /* mounting a component yields an instance */
        return ty(TY_ANY);
    if (is_unknown(callee.kind)) return ty(TY_ANY);
    terror(n->line, "value of type %s is not callable", kind_name(callee.kind));
    return ty(TY_ANY);
}

static Type check_get(Node *n, TypeEnv *env) {
    Type o = check_expr(n->as.get.object, env);
    if (is_unknown(o.kind) || o.kind == TY_NIL) return ty(TY_ANY);
    if (o.kind == TY_STRUCT) {
        NodeList *fs = &o.decl->as.struct_decl.fields;
        for (int i = 0; i < fs->count; i++)
            if (strcmp(fs->items[i]->as.param.name, n->as.get.name) == 0)
                return type_from_name(fs->items[i]->as.param.param_type);
        terror(n->line, "struct '%s' has no field '%s'", struct_name(o), n->as.get.name);
        return ty(TY_ANY);
    }
    terror(n->line, "cannot read field '%s' of %s", n->as.get.name, kind_name(o.kind));
    return ty(TY_ANY);
}

static Type check_binary(Node *n, TypeEnv *env) {
    const char *op = n->as.binary.op;
    Type l = check_expr(n->as.binary.left, env);
    Type r = check_expr(n->as.binary.right, env);

    if (!strcmp(op, "&&") || !strcmp(op, "||")) return ty(TY_BOOL);
    if (!strcmp(op, "??")) return ty(TY_ANY);
    if (!strcmp(op, "==") || !strcmp(op, "!=")) return ty(TY_BOOL);

    if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) {
        if (!is_unknown(l.kind) && !is_unknown(r.kind) && (!is_num(l.kind) || !is_num(r.kind)))
            terror(n->line, "'%s' needs numbers, got %s and %s",
                   op, kind_name(l.kind), kind_name(r.kind));
        return ty(TY_BOOL);
    }
    if (is_unknown(l.kind) || is_unknown(r.kind)) return ty(TY_ANY);
    if (!strcmp(op, "+") && l.kind == TY_STRING && r.kind == TY_STRING) return ty(TY_STRING);
    if (!strcmp(op, "%")) {
        if (l.kind != TY_INT || r.kind != TY_INT)
            terror(n->line, "'%%' needs integers, got %s and %s",
                   kind_name(l.kind), kind_name(r.kind));
        return ty(TY_INT);
    }
    if (!is_num(l.kind) || !is_num(r.kind)) {
        terror(n->line, "'%s' needs numbers%s, got %s and %s", op,
               op[0] == '+' ? " or strings" : "", kind_name(l.kind), kind_name(r.kind));
        return ty(TY_ANY);
    }
    return ty((l.kind == TY_INT && r.kind == TY_INT) ? TY_INT : TY_FLOAT);
}

static Type check_assign(Node *n, TypeEnv *env) {
    Node *target = n->as.assign.target;
    Type vt = check_expr(n->as.assign.value, env);
    int compound = n->as.assign.op[0] != '=';

    if (target->type == NODE_GET) {
        Type o = check_expr(target->as.get.object, env);
        if (o.kind == TY_STRUCT) {
            NodeList *fs = &o.decl->as.struct_decl.fields;
            for (int i = 0; i < fs->count; i++)
                if (strcmp(fs->items[i]->as.param.name, target->as.get.name) == 0) {
                    Type ft = type_from_name(fs->items[i]->as.param.param_type);
                    if (!compatible(ft, vt))
                        terror(n->line, "cannot assign %s to field '%s' of type %s",
                               kind_name(vt.kind), target->as.get.name, kind_name(ft.kind));
                    return vt;
                }
            terror(n->line, "struct '%s' has no field '%s'", struct_name(o), target->as.get.name);
        }
        return vt;
    }
    if (target->type == NODE_INDEX) {
        Type c = check_expr(target->as.index.collection, env);
        Type idx = check_expr(target->as.index.index, env);
        if (!is_unknown(c.kind) && c.kind != TY_LIST)
            terror(n->line, "cannot index-assign a value of type %s", kind_name(c.kind));
        if (!is_unknown(idx.kind) && idx.kind != TY_INT)
            terror(n->line, "list index must be Int, got %s", kind_name(idx.kind));
        if (compound && !is_unknown(vt.kind) && !is_num(vt.kind))
            terror(n->line, "'%s' needs a number", n->as.assign.op);
        return vt;
    }
    if (target->type == NODE_IDENT) {
        Type tt;
        if (!te_get(env, target->as.ident.name, &tt)) {
            terror(n->line, "undefined variable '%s'", target->as.ident.name);
            return vt;
        }
        target->ty = aty_of(tt.kind);   /* annotate target so codegen knows its C type */
        if (compound && !is_unknown(tt.kind) && !is_num(tt.kind))
            terror(n->line, "'%s' needs a number", n->as.assign.op);
        else if (!compound && !compatible(tt, vt))
            terror(n->line, "cannot assign %s to variable of type %s",
                   kind_name(vt.kind), kind_name(tt.kind));
        return vt;
    }
    terror(n->line, "invalid assignment target");
    return vt;
}

static Type check_match(Node *n, TypeEnv *env) {
    Type subj = check_expr(n->as.match.subject, env);
    Type result = ty(TY_ANY);
    int have = 0;
    NodeList *arms = &n->as.match.arms;
    for (int i = 0; i < arms->count; i++) {
        Node *arm = arms->items[i];
        Type bt;
        if (arm->as.match_arm.kind == 1) {           /* literal pattern */
            Type lt = check_expr(arm->as.match_arm.literal, env);
            if (!compatible(subj, lt))
                terror(arm->line, "pattern of type %s cannot match subject of type %s",
                       kind_name(lt.kind), kind_name(subj.kind));
            bt = check_expr(arm->as.match_arm.body, env);
        } else if (arm->as.match_arm.kind == 2) {    /* binding pattern */
            TypeEnv *scope = te_new(env);
            te_define(scope, arm->as.match_arm.bind, subj);
            bt = check_expr(arm->as.match_arm.body, scope);
        } else {                                     /* wildcard */
            bt = check_expr(arm->as.match_arm.body, env);
        }
        if (!have) { result = bt; have = 1; }
        else if (!compatible(result, bt))
            terror(arm->line, "match arms have differing types (%s vs %s)",
                   kind_name(result.kind), kind_name(bt.kind));
    }
    return result;
}

/* wrapper: annotate every expression node with its codegen type, then return */
static Type check_expr(Node *n, TypeEnv *env) {
    Type t = check_expr_inner(n, env);
    n->ty = aty_of(t.kind);
    return t;
}

static Type check_expr_inner(Node *n, TypeEnv *env) {
    switch (n->type) {
        case NODE_LITERAL:
            switch (n->as.literal.kind) {
                case TOK_INT:   return ty(TY_INT);
                case TOK_FLOAT: return ty(TY_FLOAT);
                case TOK_STRING:return ty(TY_STRING);
                case TOK_TRUE: case TOK_FALSE: return ty(TY_BOOL);
                case TOK_NIL:   return ty(TY_NIL);
                default:        return ty(TY_ANY);
            }
        case NODE_IDENT: {
            Type t;
            if (te_get(env, n->as.ident.name, &t)) return t;
            terror(n->line, "undefined variable '%s'", n->as.ident.name);
            return ty(TY_ANY);
        }
        case NODE_UNARY: {
            Type t = check_expr(n->as.unary.operand, env);
            if (n->as.unary.op[0] == '!') return ty(TY_BOOL);
            if (!is_unknown(t.kind) && !is_num(t.kind))
                terror(n->line, "unary '-' needs a number, got %s", kind_name(t.kind));
            return t;
        }
        case NODE_BINARY: return check_binary(n, env);
        case NODE_ASSIGN: return check_assign(n, env);
        case NODE_CALL:   return check_call(n, env);
        case NODE_GET:    return check_get(n, env);
        case NODE_MATCH:  return check_match(n, env);
        case NODE_RANGE: {
            Type a = check_expr(n->as.range.start, env);
            Type b = check_expr(n->as.range.end, env);
            if ((!is_unknown(a.kind) && a.kind != TY_INT) ||
                (!is_unknown(b.kind) && b.kind != TY_INT))
                terror(n->line, "range bounds must be Int");
            return ty(TY_RANGE);
        }
        case NODE_LIST: {
            for (int i = 0; i < n->as.list.elements.count; i++)
                check_expr(n->as.list.elements.items[i], env);
            return ty(TY_LIST);   /* elements are kept boxed; element type is dynamic */
        }
        case NODE_INDEX: {
            Type c = check_expr(n->as.index.collection, env);
            Type i = check_expr(n->as.index.index, env);
            if (!is_unknown(c.kind) && c.kind != TY_LIST && c.kind != TY_STRING)
                terror(n->line, "cannot index a value of type %s", kind_name(c.kind));
            if (!is_unknown(i.kind) && i.kind != TY_INT)
                terror(n->line, "list index must be Int, got %s", kind_name(i.kind));
            return ty(TY_ANY);   /* element type is dynamic in v1 */
        }
        default: return ty(TY_ANY);
    }
}

static void check_stmt(Node *n, TypeEnv *env) {
    switch (n->type) {
        case NODE_BLOCK: {
            TypeEnv *scope = te_new(env);
            for (int i = 0; i < n->as.block.statements.count; i++)
                check_stmt(n->as.block.statements.items[i], scope);
            break;
        }
        case NODE_VAR_DECL: {
            Type init = n->as.var_decl.init ? check_expr(n->as.var_decl.init, env) : ty(TY_ANY);
            Type declared;
            if (n->as.var_decl.decl_type) {
                declared = type_from_name(n->as.var_decl.decl_type);
                if (n->as.var_decl.init && !compatible(declared, init))
                    terror(n->line, "'%s' is declared %s but initialised with %s",
                           n->as.var_decl.name, kind_name(declared.kind), kind_name(init.kind));
            } else {
                declared = init;
            }
            n->ty = aty_of(declared.kind);   /* storage type for codegen */
            te_define(env, n->as.var_decl.name, declared);
            break;
        }
        case NODE_FN_DECL:
            te_define(env, n->as.fn_decl.name, ty_fn(n));  /* nested fn: register only */
            break;
        case NODE_RETURN: {
            Type t = n->as.ret.value ? check_expr(n->as.ret.value, env) : ty(TY_VOID);
            if (!compatible(g_return, t))
                terror(n->line, "returning %s but function returns %s",
                       kind_name(t.kind), kind_name(g_return.kind));
            break;
        }
        case NODE_IF: {
            Type c = check_expr(n->as.if_stmt.cond, env);
            if (!is_unknown(c.kind) && c.kind != TY_BOOL)
                terror(n->line, "'if' condition must be Bool, got %s", kind_name(c.kind));
            check_stmt(n->as.if_stmt.then_branch, env);
            if (n->as.if_stmt.else_branch) check_stmt(n->as.if_stmt.else_branch, env);
            break;
        }
        case NODE_WHILE: {
            Type c = check_expr(n->as.while_stmt.cond, env);
            if (!is_unknown(c.kind) && c.kind != TY_BOOL)
                terror(n->line, "'while' condition must be Bool, got %s", kind_name(c.kind));
            check_stmt(n->as.while_stmt.body, env);
            break;
        }
        case NODE_FOR: {
            Type it = check_expr(n->as.for_stmt.iterable, env);
            if (!is_unknown(it.kind) && it.kind != TY_RANGE && it.kind != TY_LIST)
                terror(n->line, "'for ... in' needs a Range or List, got %s", kind_name(it.kind));
            TypeEnv *scope = te_new(env);
            /* range yields Int; a list yields dynamically-typed (boxed) elements */
            te_define(scope, n->as.for_stmt.var_name,
                      it.kind == TY_RANGE ? ty(TY_INT) : ty(TY_ANY));
            check_stmt(n->as.for_stmt.body, scope);
            break;
        }
        case NODE_EXPR_STMT:
            check_expr(n->as.expr_stmt.expr, env);
            break;
        default:  /* imports, components, structs: not checked here */
            break;
    }
}

static void check_fn(Node *decl) {
    TypeEnv *scope = te_new(g_globals);
    NodeList *ps = &decl->as.fn_decl.params;
    for (int i = 0; i < ps->count; i++) {
        Type pt = type_from_name(ps->items[i]->as.param.param_type);
        ps->items[i]->ty = aty_of(pt.kind);     /* param C type for codegen */
        te_define(scope, ps->items[i]->as.param.name, pt);
    }
    g_return = decl->as.fn_decl.ret_type
             ? type_from_name(decl->as.fn_decl.ret_type) : ty(TY_ANY);
    decl->ty = aty_of(g_return.kind);            /* return C type for codegen */
    check_stmt(decl->as.fn_decl.body, scope);
}

/* ---------- entry point ---------- */

int typecheck(Node *program) {
    g_errors = 0;
    g_globals = te_new(NULL);
    te_define(g_globals, "print", ty_fn(NULL));  /* builtin, variadic */
    te_define(g_globals, "len", ty_fn(NULL));    /* builtin: List|String -> Int */
    te_define(g_globals, "push", ty_fn(NULL));   /* builtin: (List, T) -> Nil   */
    te_define(g_globals, "render", ty_fn(NULL)); /* builtin: Component -> Nil   */

    NodeList *decls = &program->as.program.declarations;

    /* pass 1a: register fn & struct names (so recursion & forward refs resolve) */
    for (int i = 0; i < decls->count; i++) {
        Node *d = decls->items[i];
        if (d->type == NODE_FN_DECL)
            te_define(g_globals, d->as.fn_decl.name, ty_fn(d));
        else if (d->type == NODE_STRUCT)
            te_define(g_globals, d->as.struct_decl.name, ty_struct_def(d));
        else if (d->type == NODE_COMPONENT)
            te_define(g_globals, d->as.component.name, ty_component_def(d));
    }

    /* pass 1b: type each global from its declaration/initialiser and register
     * that exact type, so a global's storage (d->ty) and every USE (resolved
     * via the env) agree — codegen emits consistent native-or-boxed C. */
    for (int i = 0; i < decls->count; i++) {
        Node *d = decls->items[i];
        if (d->type != NODE_VAR_DECL) continue;
        Type t;
        if (d->as.var_decl.decl_type) {
            t = type_from_name(d->as.var_decl.decl_type);
            if (d->as.var_decl.init) {
                Type init = check_expr(d->as.var_decl.init, g_globals);
                if (!compatible(t, init))
                    terror(d->line, "'%s' is declared %s but initialised with %s",
                           d->as.var_decl.name, kind_name(t.kind), kind_name(init.kind));
            }
        } else {
            t = d->as.var_decl.init ? check_expr(d->as.var_decl.init, g_globals) : ty(TY_ANY);
        }
        d->ty = aty_of(t.kind);
        te_define(g_globals, d->as.var_decl.name, t);
    }

    /* pass 2: check function bodies (globals now have their real types) */
    for (int i = 0; i < decls->count; i++)
        if (decls->items[i]->type == NODE_FN_DECL)
            check_fn(decls->items[i]);

    return g_errors;
}
