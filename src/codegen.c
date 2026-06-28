#include "codegen.h"

#include <stdlib.h>
#include <string.h>

/* Backend strategy: transpile to C. Where the type checker proved a concrete
 * scalar type (Int/Float/Bool), emit NATIVE C types (long long/double/int) and
 * native operators; everywhere else fall back to the tagged-value runtime "AV".
 * A single chokepoint (gen_expr_as) inserts every box/unbox conversion, so the
 * boundaries (print, string interpolation, ??, boxed calls) stay correct.
 *
 * This makes hot numeric code lower to the same C a human would write (so it
 * matches hand-C / beats Dart AOT) and makes that speed robust rather than
 * dependent on the optimizer scalarizing AV. Later phases add ARC + struct
 * fields + monomorphization (see docs/PERFORMANCE_AND_MOBILE_ROADMAP). */

static int g_unsupported;
static int g_tmp;          /* unique-name counter for loop temporaries */
static Node *g_program;    /* for resolving callee signatures */
static int g_fn_ret;       /* CgTy the current function returns */

static int gen_expr(Node *n, FILE *o);            /* returns the CgTy it emitted */
static void gen_expr_as(Node *n, int want, FILE *o);
static void gen_stmt(Node *n, FILE *o, int ind);

static void indent(FILE *o, int n) { for (int i = 0; i < n; i++) fputs("    ", o); }

static int is_native(int ty)     { return ty == CG_INT || ty == CG_FLOAT || ty == CG_BOOL; }
static int is_native_num(int ty) { return ty == CG_INT || ty == CG_FLOAT; }
static int norm(int ty)          { return is_native(ty) ? ty : CG_OTHER; }

static const char *cty(int ty) {
    switch (ty) {
        case CG_INT:   return "long long";
        case CG_FLOAT: return "double";
        case CG_BOOL:  return "_Bool";   /* distinct from int so _Generic (AV_OF) routes Bool -> av_bool */
        default:       return "AV";
    }
}

static Node *find_fn(const char *name) {
    NodeList *d = &g_program->as.program.declarations;
    for (int i = 0; i < d->count; i++)
        if (d->items[i]->type == NODE_FN_DECL &&
            strcmp(d->items[i]->as.fn_decl.name, name) == 0)
            return d->items[i];
    return NULL;
}

/* Each parameter and the return are typed INDEPENDENTLY: a scalar param/return
 * is native (cty -> long long/double/_Bool), anything else is AV. Storage uses
 * cty(), uses report norm() — consistent for every type, so native and boxed
 * code interoperate through gen_expr_as without an all-or-nothing ABI. */

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
    fputs("static AV av_inti(int i){ AV v; v.k=AV_INT; v.u.i=i; return v; }\n", o);
    fputs("static AV av_float(double f){ AV v; v.k=AV_FLOAT; v.u.f=f; return v; }\n", o);
    fputs("static AV av_bool(int b){ AV v; v.k=AV_BOOL; v.u.b=b; return v; }\n", o);
    fputs("static AV av_str(const char *s){ AV v; v.k=AV_STR; v.u.s=s; return v; }\n", o);
    fputs("static AV av_id(AV v){ return v; }\n", o);
    /* AV_OF: box any scalar C value (used by string interpolation) */
    fputs("#define AV_OF(x) _Generic((x), AV: av_id, _Bool: av_bool, long long: av_int, "
          "int: av_inti, double: av_float, const char *: av_str, char *: av_str)(x)\n", o);
    fputs("static int av_isnum(AV v){ return v.k==AV_INT||v.k==AV_FLOAT; }\n", o);
    fputs("static double av_num(AV v){ return v.k==AV_INT?(double)v.u.i:v.u.f; }\n", o);
    fputs("static long long av_as_int(AV v){ return v.k==AV_INT?v.u.i:(long long)av_num(v); }\n", o);
    fputs("static int av_truthy(AV v){ if(v.k==AV_NIL)return 0; if(v.k==AV_BOOL)return v.u.b; if(v.k==AV_INT)return v.u.i!=0; if(v.k==AV_FLOAT)return v.u.f!=0.0; return 1; }\n", o);
    fputs("static AV av_add(AV a, AV b){ if(a.k==AV_STR&&b.k==AV_STR){ char *s=malloc(strlen(a.u.s)+strlen(b.u.s)+1); strcpy(s,a.u.s); strcat(s,b.u.s); return av_str(s);} if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i+b.u.i); return av_float(av_num(a)+av_num(b)); }\n", o);
    fputs("static AV av_sub(AV a, AV b){ if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i-b.u.i); return av_float(av_num(a)-av_num(b)); }\n", o);
    fputs("static AV av_mul(AV a, AV b){ if(a.k==AV_INT&&b.k==AV_INT)return av_int(a.u.i*b.u.i); return av_float(av_num(a)*av_num(b)); }\n", o);
    /* integer / and % match the interpreter: a clean runtime error on zero */
    fputs("static long long abyss_idiv(long long a, long long b){ if(b==0){ fprintf(stderr,\"abyss runtime error: division by zero\\n\"); exit(70);} return a/b; }\n", o);
    fputs("static long long abyss_imod(long long a, long long b){ if(b==0){ fprintf(stderr,\"abyss runtime error: modulo by zero\\n\"); exit(70);} return a%b; }\n", o);
    fputs("static AV av_div(AV a, AV b){ if(a.k==AV_INT&&b.k==AV_INT)return av_int(abyss_idiv(a.u.i,b.u.i)); return av_float(av_num(a)/av_num(b)); }\n", o);
    fputs("static AV av_mod(AV a, AV b){ return av_int(abyss_imod(a.u.i, b.u.i)); }\n", o);
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
    /* AV_OF boxes whatever C type the variable lowered to (native or AV) */
    if (pc.is_ident) { fputs("av_tostr(AV_OF(a_", o); fwrite(pc.p, 1, (size_t)pc.n, o); fputs("))", o); }
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

/* ---------- conversions (the single box/unbox chokepoint) ---------- */

static void emit_conv(FILE *o, const char *inner, int got, int want) {
    got = norm(got); want = norm(want);
    if (got == want) { fputs(inner, o); return; }
    if (want == CG_OTHER) {                       /* box native -> AV */
        if (got == CG_INT)        fprintf(o, "av_int(%s)", inner);
        else if (got == CG_FLOAT) fprintf(o, "av_float(%s)", inner);
        else if (got == CG_BOOL)  fprintf(o, "av_bool(%s)", inner);
        else                      fputs(inner, o);
        return;
    }
    if (got == CG_OTHER) {                         /* unbox AV -> native */
        if (want == CG_INT)        fprintf(o, "av_as_int(%s)", inner);
        else if (want == CG_FLOAT) fprintf(o, "av_num(%s)", inner);
        else if (want == CG_BOOL)  fprintf(o, "av_truthy(%s)", inner);
        return;
    }
    /* native -> native */
    if (want == CG_FLOAT)      fprintf(o, "(double)(%s)", inner);
    else if (want == CG_INT)   fprintf(o, "(long long)(%s)", inner);
    else if (want == CG_BOOL)  fprintf(o, "((%s)!=0)", inner);
}

static void gen_expr_as(Node *n, int want, FILE *o) {
    char *buf = NULL;
    size_t sz = 0;
    FILE *m = open_memstream(&buf, &sz);
    int got = gen_expr(n, m);
    fclose(m);
    emit_conv(o, buf, got, want);
    free(buf);
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
    return "av_add";
}

static int gen_binary(Node *n, FILE *o) {
    const char *op = n->as.binary.op;
    Node *L = n->as.binary.left, *R = n->as.binary.right;
    int lt = norm(L->ty), rt = norm(R->ty);

    if (!strcmp(op, "&&") || !strcmp(op, "||")) {
        fputc('(', o); gen_expr_as(L, CG_BOOL, o);
        fputs(op[0] == '&' ? " && " : " || ", o);
        gen_expr_as(R, CG_BOOL, o); fputc(')', o);
        return CG_BOOL;
    }
    if (!strcmp(op, "??")) {
        /* short-circuit: evaluate R only when L is nil (matches the interpreter).
         * statement-expression keeps L single-evaluated; clang/gcc support it. */
        int id = g_tmp++;
        fprintf(o, "({ AV _q%d = ", id); gen_expr_as(L, CG_OTHER, o);
        fprintf(o, "; _q%d.k==AV_NIL ? ", id); gen_expr_as(R, CG_OTHER, o);
        fprintf(o, " : _q%d; })", id);
        return CG_OTHER;
    }
    int relational = !strcmp(op, "<") || !strcmp(op, ">") ||
                     !strcmp(op, "<=") || !strcmp(op, ">=");
    int equality = !strcmp(op, "==") || !strcmp(op, "!=");

    if (equality) {                               /* kept boxed in v1 */
        fprintf(o, "%s(", binop_fn(op));
        gen_expr_as(L, CG_OTHER, o); fputs(", ", o); gen_expr_as(R, CG_OTHER, o);
        fputc(')', o);
        return CG_OTHER;
    }

    int native_num = is_native_num(lt) && is_native_num(rt);
    int common = (lt == CG_FLOAT || rt == CG_FLOAT) ? CG_FLOAT : CG_INT;

    if (relational) {
        if (native_num) {
            fputc('(', o); gen_expr_as(L, common, o);
            fprintf(o, " %s ", op); gen_expr_as(R, common, o); fputc(')', o);
            return CG_BOOL;
        }
    } else if (!strcmp(op, "%")) {
        if (lt == CG_INT && rt == CG_INT) {   /* guarded, like the interpreter */
            fputs("abyss_imod(", o); gen_expr_as(L, CG_INT, o); fputs(", ", o);
            gen_expr_as(R, CG_INT, o); fputc(')', o);
            return CG_INT;
        }
    } else if (!strcmp(op, "/") && native_num && common == CG_INT) {
        fputs("abyss_idiv(", o); gen_expr_as(L, CG_INT, o); fputs(", ", o);
        gen_expr_as(R, CG_INT, o); fputc(')', o);
        return CG_INT;
    } else {  /* + - * , and float / */
        if (native_num) {
            fputc('(', o); gen_expr_as(L, common, o);
            fprintf(o, " %s ", op); gen_expr_as(R, common, o); fputc(')', o);
            return common;
        }
    }

    /* boxed fallback (also handles string + string, mixed/unknown types) */
    fprintf(o, "%s(", binop_fn(op));
    gen_expr_as(L, CG_OTHER, o); fputs(", ", o); gen_expr_as(R, CG_OTHER, o);
    fputc(')', o);
    return CG_OTHER;
}

static int gen_expr(Node *n, FILE *o) {
    switch (n->type) {
        case NODE_LITERAL:
            switch (n->as.literal.kind) {
                case TOK_INT:   fputs(n->as.literal.text, o); return CG_INT;
                case TOK_FLOAT: fputs(n->as.literal.text, o); return CG_FLOAT;
                case TOK_TRUE:  fputs("1", o); return CG_BOOL;
                case TOK_FALSE: fputs("0", o); return CG_BOOL;
                case TOK_NIL:   fputs("av_nil()", o); return CG_OTHER;
                case TOK_STRING: gen_string(n, o); return CG_OTHER;
                default: fputs("av_nil()", o); return CG_OTHER;
            }
        case NODE_IDENT:
            fprintf(o, "a_%s", n->as.ident.name);
            return norm(n->ty);
        case NODE_UNARY:
            if (n->as.unary.op[0] == '!') {
                fputs("(!", o); gen_expr_as(n->as.unary.operand, CG_BOOL, o); fputc(')', o);
                return CG_BOOL;
            }
            if (is_native_num(norm(n->ty))) {
                fputs("(-", o); gen_expr_as(n->as.unary.operand, norm(n->ty), o); fputc(')', o);
                return norm(n->ty);
            }
            fputs("av_neg(", o); gen_expr_as(n->as.unary.operand, CG_OTHER, o); fputc(')', o);
            return CG_OTHER;
        case NODE_BINARY:
            return gen_binary(n, o);
        case NODE_ASSIGN: {
            Node *tgt = n->as.assign.target;
            if (tgt->type != NODE_IDENT) { g_unsupported++; fputs("av_nil()", o); return CG_OTHER; }
            const char *name = tgt->as.ident.name;
            const char *op = n->as.assign.op;
            int tt = norm(tgt->ty);
            if (op[0] == '=') {
                fprintf(o, "(a_%s = ", name); gen_expr_as(n->as.assign.value, tt, o); fputc(')', o);
                return tt;
            }
            if (is_native(tt)) {                       /* native += / -= */
                fprintf(o, "(a_%s %s ", name, op); gen_expr_as(n->as.assign.value, tt, o); fputc(')', o);
                return tt;
            }
            /* boxed += / -= */
            const char *fn = op[0] == '+' ? "av_add" : "av_sub";
            fprintf(o, "(a_%s = %s(a_%s, ", name, fn, name);
            gen_expr_as(n->as.assign.value, CG_OTHER, o); fputs("))", o);
            return CG_OTHER;
        }
        case NODE_CALL: {
            Node *callee = n->as.call.callee;
            NodeList *args = &n->as.call.args;
            if (callee->type == NODE_IDENT && !strcmp(callee->as.ident.name, "print")) {
                fprintf(o, "av_print(%d", args->count);
                for (int i = 0; i < args->count; i++) { fputs(", ", o); gen_expr_as(args->items[i], CG_OTHER, o); }
                fputc(')', o);
                return CG_OTHER;
            }
            if (callee->type == NODE_IDENT) {
                Node *fn = find_fn(callee->as.ident.name);
                if (!fn) {   /* struct constructor or other non-fn: not in backend yet */
                    g_unsupported++; fputs("av_nil()", o); return CG_OTHER;
                }
                fprintf(o, "af_%s(", callee->as.ident.name);
                for (int i = 0; i < args->count; i++) {
                    if (i) fputs(", ", o);
                    int want = i < fn->as.fn_decl.params.count
                             ? norm(fn->as.fn_decl.params.items[i]->ty) : CG_OTHER;
                    gen_expr_as(args->items[i], want, o);
                }
                fputc(')', o);
                return norm(fn->ty);
            }
            g_unsupported++; fputs("av_nil()", o); return CG_OTHER;
        }
        default:  /* GET, RANGE, MATCH as a value: not yet supported by backend */
            g_unsupported++;
            fputs("av_nil()", o);
            return CG_OTHER;
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
        case NODE_VAR_DECL: {
            int t = norm(n->ty);
            indent(o, ind);
            fprintf(o, "%s a_%s = ", cty(n->ty), n->as.var_decl.name);
            if (n->as.var_decl.init) gen_expr_as(n->as.var_decl.init, t, o);
            else fputs(is_native(t) ? "0" : "av_nil()", o);
            fputs(";\n", o);
            break;
        }
        case NODE_RETURN:
            indent(o, ind); fputs("return ", o);
            if (n->as.ret.value) gen_expr_as(n->as.ret.value, g_fn_ret, o);
            else fputs(is_native(g_fn_ret) ? "0" : "av_nil()", o);
            fputs(";\n", o);
            break;
        case NODE_EXPR_STMT:
            indent(o, ind); gen_expr(n->as.expr_stmt.expr, o); fputs(";\n", o);
            break;
        case NODE_IF:
            indent(o, ind); fputs("if (", o);
            gen_expr_as(n->as.if_stmt.cond, CG_BOOL, o); fputs(") {\n", o);
            gen_stmt(n->as.if_stmt.then_branch, o, ind + 1);
            indent(o, ind); fputs("}", o);
            if (n->as.if_stmt.else_branch) {
                fputs(" else {\n", o);
                gen_stmt(n->as.if_stmt.else_branch, o, ind + 1);
                indent(o, ind); fputs("}\n", o);
            } else fputc('\n', o);
            break;
        case NODE_WHILE:
            indent(o, ind); fputs("while (", o);
            gen_expr_as(n->as.while_stmt.cond, CG_BOOL, o); fputs(") {\n", o);
            gen_stmt(n->as.while_stmt.body, o, ind + 1);
            indent(o, ind); fputs("}\n", o);
            break;
        case NODE_FOR: {
            Node *it = n->as.for_stmt.iterable;
            int id = g_tmp++;
            indent(o, ind); fputs("{\n", o);
            if (it->type == NODE_RANGE) {
                indent(o, ind + 1); fprintf(o, "long long _e%d = ", id);
                gen_expr_as(it->as.range.end, CG_INT, o); fputs(";\n", o);
                indent(o, ind + 1); fprintf(o, "for (long long _k%d = ", id);
                gen_expr_as(it->as.range.start, CG_INT, o);
                fprintf(o, "; _k%d < _e%d; _k%d++) {\n", id, id, id);
                indent(o, ind + 2);
                fprintf(o, "long long a_%s = _k%d;\n", n->as.for_stmt.var_name, id);
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
    fprintf(o, "static %s af_%s(", cty(fn->ty), fn->as.fn_decl.name);
    NodeList *ps = &fn->as.fn_decl.params;
    if (ps->count == 0) fputs("void", o);
    for (int i = 0; i < ps->count; i++) {
        if (i) fputs(", ", o);
        fprintf(o, "%s a_%s", cty(ps->items[i]->ty), ps->items[i]->as.param.name);
    }
    fputs(")", o);
}

static void gen_fn(Node *fn, FILE *o) {
    g_fn_ret = norm(fn->ty);
    gen_fn_proto(fn, o);
    fputs(" {\n", o);
    gen_stmt(fn->as.fn_decl.body, o, 1);
    fprintf(o, "    return %s;\n", is_native(g_fn_ret) ? "0" : "av_nil()");
    fputs("}\n\n", o);
}

int emit_c(Node *program, FILE *o) {
    g_unsupported = 0;
    g_tmp = 0;
    g_program = program;
    NodeList *decls = &program->as.program.declarations;

    gen_prelude(o);

    /* global variable storage (native or boxed) */
    for (int i = 0; i < decls->count; i++)
        if (decls->items[i]->type == NODE_VAR_DECL)
            fprintf(o, "static %s a_%s;\n", cty(decls->items[i]->ty),
                    decls->items[i]->as.var_decl.name);
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
            gen_expr_as(d->as.var_decl.init, norm(d->ty), o); fputs(";\n", o);
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
