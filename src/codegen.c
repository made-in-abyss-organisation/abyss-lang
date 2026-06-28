#include "codegen.h"

#include <string.h>

/* Backend strategy: transpile to C using a tagged-value runtime (`AV`).
 * This is the "fast to build" first backend — it produces a real native
 * binary via cc/clang. Later phases replace boxing with ARC + native types
 * and monomorphization for true speed (see docs/PERFORMANCE_AND_MOBILE_ROADMAP).
 *
 * Supported: functions, recursion, arithmetic, comparisons, &&/||/??, if,
 * while, for/range, var decls, assignment, string literals + interpolation,
 * and print. Not yet: structs, match, UI (emitted as nil + counted). */

static int g_unsupported;
static int g_tmp;   /* unique-name counter for loop temporaries */

static void gen_expr(Node *n, FILE *o);
static void gen_stmt(Node *n, FILE *o, int ind);

static void indent(FILE *o, int n) { for (int i = 0; i < n; i++) fputs("    ", o); }

/* ---------- the C runtime prelude ---------- */

static void gen_prelude(FILE *o) {
    fputs("#include <stdio.h>\n", o);
    fputs("#include <stdlib.h>\n", o);
    fputs("#include <string.h>\n", o);
    fputs("#include <stdarg.h>\n\n", o);
    fputs("typedef enum { AV_NIL, AV_BOOL, AV_INT, AV_FLOAT, AV_STR } AVKind;\n", o);
    fputs("typedef struct { AVKind k; union { long long i; double f; int b; const char *s; } u; } AV;\n\n", o);
    fputs("static AV av_nil(void){ AV v; v.k=AV_NIL; v.u.i=0; return v; }\n", o);
    fputs("static AV av_int(long long i){ AV v; v.k=AV_INT; v.u.i=i; return v; }\n", o);
    fputs("static AV av_float(double f){ AV v; v.k=AV_FLOAT; v.u.f=f; return v; }\n", o);
    fputs("static AV av_bool(int b){ AV v; v.k=AV_BOOL; v.u.b=b; return v; }\n", o);
    fputs("static AV av_str(const char *s){ AV v; v.k=AV_STR; v.u.s=s; return v; }\n", o);
    fputs("static int av_isnum(AV v){ return v.k==AV_INT||v.k==AV_FLOAT; }\n", o);
    fputs("static double av_num(AV v){ return v.k==AV_INT?(double)v.u.i:v.u.f; }\n", o);
    fputs("static long long av_as_int(AV v){ return v.k==AV_INT?v.u.i:(long long)av_num(v); }\n", o);
    fputs("static int av_truthy(AV v){ if(v.k==AV_NIL)return 0; if(v.k==AV_BOOL)return v.u.b; if(v.k==AV_INT)return v.u.i!=0; if(v.k==AV_FLOAT)return v.u.f!=0.0; return 1; }\n", o);
    fputs("static AV av_add(AV a, AV b){ if(a.k==AV_STR&&b.k==AV_STR){ char *s=malloc(strlen(a.u.s)+strlen(b.u.s)+1); strcpy(s,a.u.s); strcat(s,b.u.s); return av_str(s);} if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i+b.u.i); return av_float(av_num(a)+av_num(b)); }\n", o);
    fputs("static AV av_sub(AV a, AV b){ if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i-b.u.i); return av_float(av_num(a)-av_num(b)); }\n", o);
    fputs("static AV av_mul(AV a, AV b){ if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i*b.u.i); return av_float(av_num(a)*av_num(b)); }\n", o);
    fputs("static AV av_div(AV a, AV b){ if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i/b.u.i); return av_float(av_num(a)/av_num(b)); }\n", o);
    fputs("static AV av_mod(AV a, AV b){ return av_int(a.u.i % b.u.i); }\n", o);
    fputs("static AV av_lt(AV a, AV b){ return av_bool(av_num(a)<av_num(b)); }\n", o);
    fputs("static AV av_gt(AV a, AV b){ return av_bool(av_num(a)>av_num(b)); }\n", o);
    fputs("static AV av_le(AV a, AV b){ return av_bool(av_num(a)<=av_num(b)); }\n", o);
    fputs("static AV av_ge(AV a, AV b){ return av_bool(av_num(a)>=av_num(b)); }\n", o);
    fputs("static AV av_eq(AV a, AV b){ if(av_isnum(a)&&av_isnum(b))return av_bool(av_num(a)==av_num(b)); if(a.k==AV_STR&&b.k==AV_STR)return av_bool(strcmp(a.u.s,b.u.s)==0); if(a.k==AV_BOOL&&b.k==AV_BOOL)return av_bool(a.u.b==b.u.b); if(a.k==AV_NIL&&b.k==AV_NIL)return av_bool(1); return av_bool(0); }\n", o);
    fputs("static AV av_neq(AV a, AV b){ return av_bool(!av_truthy(av_eq(a,b))); }\n", o);
    fputs("static AV av_neg(AV a){ if(a.k==AV_INT)return av_int(-a.u.i); return av_float(-av_num(a)); }\n", o);
    fputs("static AV av_not(AV a){ return av_bool(!av_truthy(a)); }\n", o);
    fputs("static AV av_coalesce(AV a, AV b){ return a.k==AV_NIL?b:a; }\n", o);
    fputs("static AV av_tostr(AV v){ if(v.k==AV_STR)return v; char buf[64]; if(v.k==AV_INT)snprintf(buf,sizeof buf,\"%lld\",v.u.i); else if(v.k==AV_FLOAT)snprintf(buf,sizeof buf,\"%g\",v.u.f); else if(v.k==AV_BOOL)snprintf(buf,sizeof buf,\"%s\",v.u.b?\"true\":\"false\"); else snprintf(buf,sizeof buf,\"nil\"); char *s=malloc(strlen(buf)+1); strcpy(s,buf); return av_str(s); }\n", o);
    fputs("static void av_print1(AV v){ if(v.k==AV_NIL)printf(\"nil\"); else if(v.k==AV_BOOL)printf(\"%s\",v.u.b?\"true\":\"false\"); else if(v.k==AV_INT)printf(\"%lld\",v.u.i); else if(v.k==AV_FLOAT)printf(\"%g\",v.u.f); else if(v.k==AV_STR)printf(\"%s\",v.u.s); }\n", o);
    fputs("static AV av_print(int n, ...){ va_list ap; va_start(ap,n); for(int i=0;i<n;i++){ if(i)printf(\" \"); AV v=va_arg(ap,AV); av_print1(v);} va_end(ap); printf(\"\\n\"); return av_nil(); }\n\n", o);
}

/* ---------- string literals (with ${ident} interpolation) ---------- */

static void emit_cstr(FILE *o, const char *s, int len) {
    fputc('"', o);
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { fputc('\\', o); fputc(c, o); }
        else if (c == '\n') fputs("\\n", o);
        else if (c == '\t') fputs("\\t", o);
        else fputc(c, o);
    }
    fputc('"', o);
}

typedef struct { int is_ident; const char *p; int n; } Piece;

static void emit_piece(FILE *o, Piece pc) {
    if (pc.is_ident) { fputs("av_tostr(a_", o); fwrite(pc.p, 1, (size_t)pc.n, o); fputc(')', o); }
    else { fputs("av_str(", o); emit_cstr(o, pc.p, pc.n); fputc(')', o); }
}

static void emit_fold(FILE *o, Piece *pc, int count, int idx) {
    if (idx == count - 1) { emit_piece(o, pc[idx]); return; }
    fputs("av_add(", o); emit_piece(o, pc[idx]); fputs(", ", o);
    emit_fold(o, pc, count, idx + 1); fputc(')', o);
}

static void gen_string(Node *n, FILE *o) {
    const char *t = n->as.literal.text;     /* includes quotes */
    int len = (int)strlen(t);
    Piece pc[256];
    int npc = 0, run = 1;
    for (int i = 1; i < len - 1 && npc < 250; ) {
        if (t[i] == '$' && i + 1 < len - 1 && t[i + 1] == '{') {
            if (i > run) { pc[npc].is_ident = 0; pc[npc].p = t + run; pc[npc].n = i - run; npc++; }
            int j = i + 2;
            while (j < len - 1 && t[j] != '}') j++;
            int s = i + 2, e = j;
            while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
            while (e > s && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
            pc[npc].is_ident = 1; pc[npc].p = t + s; pc[npc].n = e - s; npc++;
            i = (j < len - 1) ? j + 1 : j;
            run = i;
        } else i++;
    }
    if (len - 1 > run) { pc[npc].is_ident = 0; pc[npc].p = t + run; pc[npc].n = len - 1 - run; npc++; }
    if (npc == 0) { fputs("av_str(\"\")", o); return; }
    emit_fold(o, pc, npc, 0);
}

/* ---------- expressions ---------- */

static const char *binop_fn(const char *op) {
    if (!strcmp(op, "+")) return "av_add";
    if (!strcmp(op, "-")) return "av_sub";
    if (!strcmp(op, "*")) return "av_mul";
    if (!strcmp(op, "/")) return "av_div";
    if (!strcmp(op, "%")) return "av_mod";
    if (!strcmp(op, "==")) return "av_eq";
    if (!strcmp(op, "!=")) return "av_neq";
    if (!strcmp(op, "<")) return "av_lt";
    if (!strcmp(op, ">")) return "av_gt";
    if (!strcmp(op, "<=")) return "av_le";
    if (!strcmp(op, ">=")) return "av_ge";
    return NULL;
}

static void gen_expr(Node *n, FILE *o) {
    switch (n->type) {
        case NODE_LITERAL:
            switch (n->as.literal.kind) {
                case TOK_INT:   fprintf(o, "av_int(%s)", n->as.literal.text); break;
                case TOK_FLOAT: fprintf(o, "av_float(%s)", n->as.literal.text); break;
                case TOK_TRUE:  fputs("av_bool(1)", o); break;
                case TOK_FALSE: fputs("av_bool(0)", o); break;
                case TOK_NIL:   fputs("av_nil()", o); break;
                case TOK_STRING: gen_string(n, o); break;
                default: fputs("av_nil()", o); break;
            }
            break;
        case NODE_IDENT:
            fprintf(o, "a_%s", n->as.ident.name);
            break;
        case NODE_UNARY:
            fputs(n->as.unary.op[0] == '!' ? "av_not(" : "av_neg(", o);
            gen_expr(n->as.unary.operand, o); fputc(')', o);
            break;
        case NODE_BINARY: {
            const char *op = n->as.binary.op;
            if (!strcmp(op, "&&")) {
                fputs("av_bool(av_truthy(", o); gen_expr(n->as.binary.left, o);
                fputs(") ? av_truthy(", o); gen_expr(n->as.binary.right, o); fputs(") : 0)", o);
            } else if (!strcmp(op, "||")) {
                fputs("av_bool(av_truthy(", o); gen_expr(n->as.binary.left, o);
                fputs(") ? 1 : av_truthy(", o); gen_expr(n->as.binary.right, o); fputs("))", o);
            } else if (!strcmp(op, "??")) {
                fputs("av_coalesce(", o); gen_expr(n->as.binary.left, o);
                fputs(", ", o); gen_expr(n->as.binary.right, o); fputc(')', o);
            } else {
                fprintf(o, "%s(", binop_fn(op));
                gen_expr(n->as.binary.left, o); fputs(", ", o);
                gen_expr(n->as.binary.right, o); fputc(')', o);
            }
            break;
        }
        case NODE_ASSIGN: {
            Node *tgt = n->as.assign.target;
            if (tgt->type != NODE_IDENT) { g_unsupported++; fputs("av_nil()", o); break; }
            const char *name = tgt->as.ident.name;
            const char *op = n->as.assign.op;
            if (op[0] == '=') {
                fprintf(o, "(a_%s = ", name); gen_expr(n->as.assign.value, o); fputc(')', o);
            } else {
                const char *fn = op[0] == '+' ? "av_add" : "av_sub";
                fprintf(o, "(a_%s = %s(a_%s, ", name, fn, name);
                gen_expr(n->as.assign.value, o); fputs("))", o);
            }
            break;
        }
        case NODE_CALL: {
            Node *callee = n->as.call.callee;
            NodeList *args = &n->as.call.args;
            if (callee->type == NODE_IDENT && !strcmp(callee->as.ident.name, "print")) {
                fprintf(o, "av_print(%d", args->count);
                for (int i = 0; i < args->count; i++) { fputs(", ", o); gen_expr(args->items[i], o); }
                fputc(')', o);
            } else if (callee->type == NODE_IDENT) {
                fprintf(o, "af_%s(", callee->as.ident.name);
                for (int i = 0; i < args->count; i++) {
                    if (i) fputs(", ", o);
                    gen_expr(args->items[i], o);
                }
                fputc(')', o);
            } else { g_unsupported++; fputs("av_nil()", o); }
            break;
        }
        default:  /* GET, RANGE, MATCH as a value: not yet supported by backend */
            g_unsupported++;
            fputs("av_nil()", o);
            break;
    }
}

/* ---------- statements ---------- */

static void gen_stmt(Node *n, FILE *o, int ind) {
    switch (n->type) {
        case NODE_BLOCK:
            indent(o, ind); fputs("{\n", o);
            for (int i = 0; i < n->as.block.statements.count; i++)
                gen_stmt(n->as.block.statements.items[i], o, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        case NODE_VAR_DECL:
            indent(o, ind);
            fprintf(o, "AV a_%s = ", n->as.var_decl.name);
            if (n->as.var_decl.init) gen_expr(n->as.var_decl.init, o);
            else fputs("av_nil()", o);
            fputs(";\n", o);
            break;
        case NODE_RETURN:
            indent(o, ind); fputs("return ", o);
            if (n->as.ret.value) gen_expr(n->as.ret.value, o);
            else fputs("av_nil()", o);
            fputs(";\n", o);
            break;
        case NODE_EXPR_STMT:
            indent(o, ind); gen_expr(n->as.expr_stmt.expr, o); fputs(";\n", o);
            break;
        case NODE_IF:
            indent(o, ind); fputs("if (av_truthy(", o);
            gen_expr(n->as.if_stmt.cond, o); fputs(")) {\n", o);
            gen_stmt(n->as.if_stmt.then_branch, o, ind + 1);
            indent(o, ind); fputs("}", o);
            if (n->as.if_stmt.else_branch) {
                fputs(" else {\n", o);
                gen_stmt(n->as.if_stmt.else_branch, o, ind + 1);
                indent(o, ind); fputs("}\n", o);
            } else fputc('\n', o);
            break;
        case NODE_WHILE:
            indent(o, ind); fputs("while (av_truthy(", o);
            gen_expr(n->as.while_stmt.cond, o); fputs(")) {\n", o);
            gen_stmt(n->as.while_stmt.body, o, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        case NODE_FOR: {
            Node *it = n->as.for_stmt.iterable;
            int id = g_tmp++;
            indent(o, ind); fputs("{\n", o);
            if (it->type == NODE_RANGE) {
                indent(o, ind + 1); fprintf(o, "long long _e%d = av_as_int(", id);
                gen_expr(it->as.range.end, o); fputs(");\n", o);
                indent(o, ind + 1); fprintf(o, "for (long long _k%d = av_as_int(", id);
                gen_expr(it->as.range.start, o);
                fprintf(o, "); _k%d < _e%d; _k%d++) {\n", id, id, id);
                indent(o, ind + 2);
                fprintf(o, "AV a_%s = av_int(_k%d);\n", n->as.for_stmt.var_name, id);
                gen_stmt(n->as.for_stmt.body, o, ind + 2);
                indent(o, ind + 1); fputs("}\n", o);
            } else {
                g_unsupported++;
                indent(o, ind + 1); fputs("/* unsupported for-iterable */\n", o);
            }
            indent(o, ind); fputs("}\n", o);
            break;
        }
        default:  /* nested fn decls, struct, component, import: skip in a body */
            break;
    }
}

/* ---------- functions, globals, main ---------- */

static void gen_fn_proto(Node *fn, FILE *o) {
    fprintf(o, "static AV af_%s(", fn->as.fn_decl.name);
    NodeList *ps = &fn->as.fn_decl.params;
    if (ps->count == 0) fputs("void", o);
    for (int i = 0; i < ps->count; i++) {
        if (i) fputs(", ", o);
        fprintf(o, "AV a_%s", ps->items[i]->as.param.name);
    }
    fputs(")", o);
}

static void gen_fn(Node *fn, FILE *o) {
    gen_fn_proto(fn, o);
    fputs(" {\n", o);
    gen_stmt(fn->as.fn_decl.body, o, 1);
    fputs("    return av_nil();\n", o);   /* fallthrough default */
    fputs("}\n\n", o);
}

int emit_c(Node *program, FILE *o) {
    g_unsupported = 0;
    g_tmp = 0;
    NodeList *decls = &program->as.program.declarations;

    gen_prelude(o);

    /* global variable storage */
    for (int i = 0; i < decls->count; i++)
        if (decls->items[i]->type == NODE_VAR_DECL)
            fprintf(o, "static AV a_%s;\n", decls->items[i]->as.var_decl.name);
    fputc('\n', o);

    /* function prototypes (so recursion & forward references compile) */
    for (int i = 0; i < decls->count; i++)
        if (decls->items[i]->type == NODE_FN_DECL) {
            gen_fn_proto(decls->items[i], o); fputs(";\n", o);
        }
    fputc('\n', o);

    /* function bodies */
    for (int i = 0; i < decls->count; i++)
        if (decls->items[i]->type == NODE_FN_DECL)
            gen_fn(decls->items[i], o);

    /* global initialisers */
    fputs("static void abyss_init_globals(void) {\n", o);
    for (int i = 0; i < decls->count; i++) {
        Node *d = decls->items[i];
        if (d->type == NODE_VAR_DECL && d->as.var_decl.init) {
            fprintf(o, "    a_%s = ", d->as.var_decl.name);
            gen_expr(d->as.var_decl.init, o); fputs(";\n", o);
        }
    }
    fputs("}\n\n", o);

    fputs("int main(void) {\n", o);
    fputs("    abyss_init_globals();\n", o);
    fputs("    af_main();\n", o);
    fputs("    return 0;\n", o);
    fputs("}\n", o);

    return g_unsupported;
}
