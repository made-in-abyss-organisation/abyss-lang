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
static int gen_match(Node *n, FILE *o);
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

static Node *find_struct(const char *name) {
    NodeList *d = &g_program->as.program.declarations;
    for (int i = 0; i < d->count; i++)
        if (d->items[i]->type == NODE_STRUCT &&
            strcmp(d->items[i]->as.struct_decl.name, name) == 0)
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
    fputs("typedef enum { AV_NIL, AV_BOOL, AV_INT, AV_FLOAT, AV_STR, AV_STRUCT, AV_LIST } AVKind;\n", o);
    fputs("typedef struct AObj AObj;\n", o);
    fputs("typedef struct AList AList;\n", o);
    fputs("typedef struct { AVKind k; union { long long i; double f; int b; const char *s; AObj *o; AList *l; } u; } AV;\n", o);
    fputs("struct AObj { const char *name; int n; const char **names; AV *vals; };\n", o);
    fputs("struct AList { int n; int cap; AV *items; };\n\n", o);
    fputs("static AV av_nil(void){ AV v; v.k=AV_NIL; v.u.i=0; return v; }\n", o);
    fputs("static AV av_int(long long i){ AV v; v.k=AV_INT; v.u.i=i; return v; }\n", o);
    fputs("static AV av_inti(int i){ AV v; v.k=AV_INT; v.u.i=i; return v; }\n", o);
    fputs("static AV av_float(double f){ AV v; v.k=AV_FLOAT; v.u.f=f; return v; }\n", o);
    fputs("static AV av_bool(int b){ AV v; v.k=AV_BOOL; v.u.b=b; return v; }\n", o);
    fputs("static AV av_str(const char *s){ AV v; v.k=AV_STR; v.u.s=s; return v; }\n", o);
    fputs("static AV av_id(AV v){ return v; }\n", o);
    /* struct instances: a heap object carrying its type name + named fields.
     * Reference semantics (the AV holds a pointer), matching the interpreter. */
    fputs("static AV av_obj(const char *name, int n, const char **names, AV *vals){ AObj *o=malloc(sizeof(AObj)); o->name=name; o->n=n; o->names=malloc(sizeof(char*)*(n?n:1)); o->vals=malloc(sizeof(AV)*(n?n:1)); for(int i=0;i<n;i++){ o->names[i]=names[i]; o->vals[i]=vals[i]; } AV v; v.k=AV_STRUCT; v.u.o=o; return v; }\n", o);
    fputs("static AV av_get(AV v, const char *name, int safe){ if(safe&&v.k==AV_NIL)return av_nil(); if(v.k!=AV_STRUCT){ fprintf(stderr,\"abyss runtime error: cannot read field '%s' of a non-struct value\\n\",name); exit(70);} AObj *o=v.u.o; for(int i=0;i<o->n;i++) if(strcmp(o->names[i],name)==0) return o->vals[i]; fprintf(stderr,\"abyss runtime error: struct '%s' has no field '%s'\\n\",o->name,name); exit(70); }\n", o);
    fputs("static AV av_set(AV v, const char *name, AV val){ if(v.k!=AV_STRUCT){ fprintf(stderr,\"abyss runtime error: cannot assign field of a non-struct value\\n\"); exit(70);} AObj *o=v.u.o; for(int i=0;i<o->n;i++) if(strcmp(o->names[i],name)==0){ o->vals[i]=val; return val; } fprintf(stderr,\"abyss runtime error: struct '%s' has no field '%s'\\n\",o->name,name); exit(70); }\n", o);
    /* AV_OF: box any scalar C value (used by string interpolation) */
    fputs("#define AV_OF(x) _Generic((x), AV: av_id, _Bool: av_bool, long long: av_int, "
          "int: av_inti, double: av_float, const char *: av_str, char *: av_str)(x)\n", o);
    fputs("static int av_isnum(AV v){ return v.k==AV_INT||v.k==AV_FLOAT; }\n", o);
    fputs("static double av_num(AV v){ return v.k==AV_INT?(double)v.u.i:v.u.f; }\n", o);
    fputs("static long long av_as_int(AV v){ return v.k==AV_INT?v.u.i:(long long)av_num(v); }\n", o);
    /* lists: a heap object holding a growable array of AV. Reference semantics
     * (the AV holds a pointer), matching the interpreter's AbyssList. */
    fputs("static AV av_list(int n, AV *items){ AList *l=malloc(sizeof(AList)); l->n=n; l->cap=n?n:1; l->items=malloc(sizeof(AV)*l->cap); for(int i=0;i<n;i++) l->items[i]=items[i]; AV v; v.k=AV_LIST; v.u.l=l; return v; }\n", o);
    fputs("static AV av_index(AV c, AV i){ long long idx=av_as_int(i); if(c.k!=AV_LIST){ fprintf(stderr,\"abyss runtime error: cannot index a non-list value\\n\"); exit(70);} AList *l=c.u.l; if(idx<0||idx>=l->n){ fprintf(stderr,\"abyss runtime error: list index out of range\\n\"); exit(70);} return l->items[idx]; }\n", o);
    fputs("static AV av_setidx(AV c, AV i, AV val){ long long idx=av_as_int(i); if(c.k!=AV_LIST){ fprintf(stderr,\"abyss runtime error: cannot index-assign a non-list value\\n\"); exit(70);} AList *l=c.u.l; if(idx<0||idx>=l->n){ fprintf(stderr,\"abyss runtime error: list index out of range\\n\"); exit(70);} l->items[idx]=val; return val; }\n", o);
    fputs("static AV av_len(AV v){ if(v.k==AV_LIST)return av_int(v.u.l->n); if(v.k==AV_STR)return av_int((long long)strlen(v.u.s)); fprintf(stderr,\"abyss runtime error: len() needs a list or string\\n\"); exit(70); }\n", o);
    fputs("static AV av_push(AV c, AV val){ if(c.k!=AV_LIST){ fprintf(stderr,\"abyss runtime error: push() needs a list as its first argument\\n\"); exit(70);} AList *l=c.u.l; if(l->n+1>l->cap){ l->cap=l->cap<4?4:l->cap*2; l->items=realloc(l->items,sizeof(AV)*l->cap);} l->items[l->n++]=val; return av_nil(); }\n", o);
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
    /* append a C string to a growable buffer (used to stringify lists) */
    fputs("static void av_strapp(char **buf, size_t *cap, size_t *len, const char *s){ size_t n=strlen(s); while(*len+n+1>*cap){ *cap*=2; *buf=realloc(*buf,*cap);} memcpy(*buf+*len,s,n+1); *len+=n; }\n", o);
    fputs("static AV av_tostr(AV v){ if(v.k==AV_STR)return v; if(v.k==AV_LIST){ AList *l=v.u.l; size_t cap=8,len=0; char *b=malloc(cap); b[0]=0; av_strapp(&b,&cap,&len,\"[\"); for(int i=0;i<l->n;i++){ if(i)av_strapp(&b,&cap,&len,\", \"); AV e=av_tostr(l->items[i]); av_strapp(&b,&cap,&len,e.u.s); } av_strapp(&b,&cap,&len,\"]\"); return av_str(b); } if(v.k==AV_STRUCT)return av_str(\"<fn>\"); char buf[64]; if(v.k==AV_INT)snprintf(buf,sizeof buf,\"%lld\",v.u.i); else if(v.k==AV_FLOAT)snprintf(buf,sizeof buf,\"%g\",v.u.f); else if(v.k==AV_BOOL)snprintf(buf,sizeof buf,\"%s\",v.u.b?\"true\":\"false\"); else snprintf(buf,sizeof buf,\"nil\"); char *s=malloc(strlen(buf)+1); strcpy(s,buf); return av_str(s); }\n", o);
    fputs("static void av_print1(AV v){ if(v.k==AV_NIL)printf(\"nil\"); else if(v.k==AV_BOOL)printf(\"%s\",v.u.b?\"true\":\"false\"); else if(v.k==AV_INT)printf(\"%lld\",v.u.i); else if(v.k==AV_FLOAT)printf(\"%g\",v.u.f); else if(v.k==AV_STR)printf(\"%s\",v.u.s); else if(v.k==AV_LIST){ AList *l=v.u.l; printf(\"[\"); for(int i=0;i<l->n;i++){ if(i)printf(\", \"); av_print1(l->items[i]); } printf(\"]\"); } else if(v.k==AV_STRUCT){ AObj *o=v.u.o; printf(\"%s { \",o->name); for(int i=0;i<o->n;i++){ if(i)printf(\", \"); printf(\"%s: \",o->names[i]); av_print1(o->vals[i]); } printf(\" }\"); } }\n", o);
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

/* Predict the CgTy that gen_expr() will emit for `n`, WITHOUT emitting it.
 * This MUST stay in lockstep with the return values of gen_expr()/gen_binary().
 * It lets gen_expr_as() wrap a sub-expression with the correct box/unbox
 * conversion by emitting a prefix and suffix around a *direct* gen_expr() call,
 * so we never have to capture generated text in a memory stream. (The old
 * design used open_memstream, which is POSIX/glibc-only and absent on Windows;
 * this keeps the backend portable across macOS, Linux, and Windows.) */
static int expr_cgty(Node *n) {
    switch (n->type) {
        case NODE_LITERAL:
            switch (n->as.literal.kind) {
                case TOK_INT:   return CG_INT;
                case TOK_FLOAT: return CG_FLOAT;
                case TOK_TRUE: case TOK_FALSE: return CG_BOOL;
                default:        return CG_OTHER;   /* nil, string */
            }
        case NODE_IDENT:
            return norm(n->ty);
        case NODE_UNARY:
            if (n->as.unary.op[0] == '!') return CG_BOOL;
            return is_native_num(norm(n->ty)) ? norm(n->ty) : CG_OTHER;
        case NODE_BINARY: {
            const char *op = n->as.binary.op;
            if (!strcmp(op, "&&") || !strcmp(op, "||")) return CG_BOOL;
            if (!strcmp(op, "??")) return CG_OTHER;
            if (!strcmp(op, "==") || !strcmp(op, "!=")) return CG_OTHER;
            int lt = norm(n->as.binary.left->ty);
            int rt = norm(n->as.binary.right->ty);
            int native_num = is_native_num(lt) && is_native_num(rt);
            int common = (lt == CG_FLOAT || rt == CG_FLOAT) ? CG_FLOAT : CG_INT;
            int relational = !strcmp(op, "<") || !strcmp(op, ">") ||
                             !strcmp(op, "<=") || !strcmp(op, ">=");
            if (relational) return native_num ? CG_BOOL : CG_OTHER;
            if (!strcmp(op, "%")) return (lt == CG_INT && rt == CG_INT) ? CG_INT : CG_OTHER;
            if (!strcmp(op, "/") && native_num && common == CG_INT) return CG_INT;
            return native_num ? common : CG_OTHER;   /* + - *, and float / */
        }
        case NODE_ASSIGN: {
            Node *tgt = n->as.assign.target;
            if (tgt->type != NODE_IDENT) return CG_OTHER;
            int tt = norm(tgt->ty);
            if (n->as.assign.op[0] == '=') return tt;
            return is_native(tt) ? tt : CG_OTHER;     /* compound += / -= */
        }
        case NODE_LIST:        /* list literals and subscripts are always boxed AV */
        case NODE_INDEX:
            return CG_OTHER;
        case NODE_CALL: {
            Node *callee = n->as.call.callee;
            if (callee->type == NODE_IDENT && strcmp(callee->as.ident.name, "print") != 0) {
                Node *fn = find_fn(callee->as.ident.name);
                if (fn) return norm(fn->ty);
            }
            return CG_OTHER;   /* builtins (len/push) and unknown callees: boxed */
        }
        default:
            return CG_OTHER;
    }
}

/* Emit `n` converted to the `want` CgTy. Every conversion is prefix+inner+
 * suffix, so we emit the prefix, let gen_expr() write the inner expression
 * straight to `o`, then emit the suffix — no intermediate buffer required. */
static void gen_expr_as(Node *n, int want, FILE *o) {
    int got = norm(expr_cgty(n));
    want = norm(want);
    if (got == want) { gen_expr(n, o); return; }

    const char *pre = "", *post = ")";
    if (want == CG_OTHER) {                 /* box native -> AV */
        if (got == CG_INT)        pre = "av_int(";
        else if (got == CG_FLOAT) pre = "av_float(";
        else if (got == CG_BOOL)  pre = "av_bool(";
        else { gen_expr(n, o); return; }    /* nothing to box */
    } else if (got == CG_OTHER) {           /* unbox AV -> native */
        if (want == CG_INT)        pre = "av_as_int(";
        else if (want == CG_FLOAT) pre = "av_num(";
        else                       pre = "av_truthy(";   /* CG_BOOL */
    } else {                                /* native -> native */
        if (want == CG_FLOAT)      pre = "(double)(";
        else if (want == CG_INT)   pre = "(long long)(";
        else { pre = "(("; post = ")!=0)"; }             /* CG_BOOL */
    }
    fputs(pre, o);
    gen_expr(n, o);
    fputs(post, o);
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

/* Lower a `match` expression to a statement-expression: capture the subject
 * once, then an if/else-if chain over the arms (matching the interpreter's
 * first-arm-wins order). A binding or wildcard arm always matches, so it
 * terminates the chain as the trailing `else`; arms after it are unreachable
 * (exactly as in the interpreter). Result is boxed to AV (arms may differ in
 * type), so a match expression's CgTy is CG_OTHER. */
static int gen_match(Node *n, FILE *o) {
    int id = g_tmp++;
    int sty = norm(expr_cgty(n->as.match.subject));   /* subject's emitted repr */
    fprintf(o, "({ AV _m%d = ", id);
    gen_expr_as(n->as.match.subject, CG_OTHER, o);
    fprintf(o, "; AV _r%d = av_nil(); ", id);

    NodeList *arms = &n->as.match.arms;
    int terminated = 0, emitted = 0;
    for (int i = 0; i < arms->count && !terminated; i++) {
        Node *arm = arms->items[i];
        int kind = arm->as.match_arm.kind;
        if (kind == 1) {                       /* literal pattern */
            if (emitted) fputs("else ", o);
            fprintf(o, "if (av_truthy(av_eq(_m%d, ", id);
            gen_expr_as(arm->as.match_arm.literal, CG_OTHER, o);
            fprintf(o, "))) { _r%d = ", id);
            gen_expr_as(arm->as.match_arm.body, CG_OTHER, o);
            fputs("; } ", o);
            emitted = 1;
        } else if (kind == 2) {                /* binding pattern (always matches) */
            if (emitted) fputs("else ", o);
            fputs("{ ", o);
            if (is_native(sty)) {              /* bind storage mirrors subject repr */
                const char *unbox = sty == CG_INT ? "av_as_int"
                                  : sty == CG_FLOAT ? "av_num" : "av_truthy";
                fprintf(o, "%s a_%s = %s(_m%d); ", cty(sty), arm->as.match_arm.bind, unbox, id);
            } else {
                fprintf(o, "AV a_%s = _m%d; ", arm->as.match_arm.bind, id);
            }
            fprintf(o, "_r%d = ", id);
            gen_expr_as(arm->as.match_arm.body, CG_OTHER, o);
            fputs("; } ", o);
            terminated = 1;
        } else {                               /* wildcard (always matches) */
            if (emitted) fputs("else ", o);
            fprintf(o, "{ _r%d = ", id);
            gen_expr_as(arm->as.match_arm.body, CG_OTHER, o);
            fputs("; } ", o);
            terminated = 1;
        }
    }
    if (!terminated) {
        if (emitted) fputs("else ", o);
        fputs("{ fprintf(stderr, \"abyss runtime error: no match arm matched\\n\"); exit(70); } ", o);
    }
    fprintf(o, "_r%d; })", id);
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
            if (tgt->type == NODE_GET) {             /* field assignment: p.x = v */
                const char *fname = tgt->as.get.name;
                const char *aop = n->as.assign.op;
                if (aop[0] == '=') {
                    fputs("av_set(", o); gen_expr_as(tgt->as.get.object, CG_OTHER, o);
                    fprintf(o, ", \"%s\", ", fname);
                    gen_expr_as(n->as.assign.value, CG_OTHER, o); fputc(')', o);
                    return CG_OTHER;
                }
                /* compound p.x += v : evaluate the object once (statement-expr) */
                int id = g_tmp++;
                const char *afn = aop[0] == '+' ? "av_add" : "av_sub";
                fprintf(o, "({ AV _s%d = ", id); gen_expr_as(tgt->as.get.object, CG_OTHER, o);
                fprintf(o, "; av_set(_s%d, \"%s\", %s(av_get(_s%d, \"%s\", 0), ", id, fname, afn, id, fname);
                gen_expr_as(n->as.assign.value, CG_OTHER, o); fputs(")); })", o);
                return CG_OTHER;
            }
            if (tgt->type == NODE_INDEX) {           /* element assignment: xs[i] = v */
                const char *aop = n->as.assign.op;
                if (aop[0] == '=') {
                    fputs("av_setidx(", o); gen_expr_as(tgt->as.index.collection, CG_OTHER, o);
                    fputs(", ", o); gen_expr_as(tgt->as.index.index, CG_OTHER, o);
                    fputs(", ", o); gen_expr_as(n->as.assign.value, CG_OTHER, o); fputc(')', o);
                    return CG_OTHER;
                }
                /* compound xs[i] += v : evaluate collection & index once (statement-expr) */
                int id = g_tmp++;
                const char *afn = aop[0] == '+' ? "av_add" : "av_sub";
                fprintf(o, "({ AV _c%d = ", id); gen_expr_as(tgt->as.index.collection, CG_OTHER, o);
                fprintf(o, "; AV _i%d = ", id); gen_expr_as(tgt->as.index.index, CG_OTHER, o);
                fprintf(o, "; av_setidx(_c%d, _i%d, %s(av_index(_c%d, _i%d), ", id, id, afn, id, id);
                gen_expr_as(n->as.assign.value, CG_OTHER, o); fputs(")); })", o);
                return CG_OTHER;
            }
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
            if (callee->type == NODE_IDENT && !strcmp(callee->as.ident.name, "len") &&
                args->count == 1) {
                fputs("av_len(", o); gen_expr_as(args->items[0], CG_OTHER, o); fputc(')', o);
                return CG_OTHER;
            }
            if (callee->type == NODE_IDENT && !strcmp(callee->as.ident.name, "push") &&
                args->count == 2) {
                fputs("av_push(", o); gen_expr_as(args->items[0], CG_OTHER, o);
                fputs(", ", o); gen_expr_as(args->items[1], CG_OTHER, o); fputc(')', o);
                return CG_OTHER;
            }
            if (callee->type == NODE_IDENT) {
                Node *fn = find_fn(callee->as.ident.name);
                if (!fn) {
                    Node *st = find_struct(callee->as.ident.name);
                    if (st) {   /* struct constructor: Point(3, 4) */
                        NodeList *fs = &st->as.struct_decl.fields;
                        int cnt = args->count;
                        fprintf(o, "av_obj(\"%s\", %d, ", st->as.struct_decl.name, cnt);
                        if (cnt == 0) { fputs("NULL, NULL)", o); return CG_OTHER; }
                        fputs("(const char*[]){", o);
                        for (int i = 0; i < cnt; i++) {
                            if (i) fputs(", ", o);
                            fprintf(o, "\"%s\"", i < fs->count ? fs->items[i]->as.param.name : "?");
                        }
                        fputs("}, (AV[]){", o);
                        for (int i = 0; i < cnt; i++) {
                            if (i) fputs(", ", o);
                            gen_expr_as(args->items[i], CG_OTHER, o);
                        }
                        fputs("})", o);
                        return CG_OTHER;
                    }
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
        case NODE_GET:
            fputs("av_get(", o); gen_expr_as(n->as.get.object, CG_OTHER, o);
            fprintf(o, ", \"%s\", %d)", n->as.get.name, n->as.get.safe ? 1 : 0);
            return CG_OTHER;
        case NODE_LIST: {
            int cnt = n->as.list.elements.count;
            if (cnt == 0) { fputs("av_list(0, NULL)", o); return CG_OTHER; }
            fprintf(o, "av_list(%d, (AV[]){", cnt);
            for (int i = 0; i < cnt; i++) {
                if (i) fputs(", ", o);
                gen_expr_as(n->as.list.elements.items[i], CG_OTHER, o);
            }
            fputs("})", o);
            return CG_OTHER;
        }
        case NODE_INDEX:
            fputs("av_index(", o); gen_expr_as(n->as.index.collection, CG_OTHER, o);
            fputs(", ", o); gen_expr_as(n->as.index.index, CG_OTHER, o); fputc(')', o);
            return CG_OTHER;
        case NODE_MATCH:
            return gen_match(n, o);
        default:  /* RANGE as a bare value, etc.: not supported by the backend */
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
            } else {   /* iterate a list (or any boxed iterable, checked at runtime) */
                indent(o, ind + 1); fprintf(o, "AV _lst%d = ", id);
                gen_expr_as(it, CG_OTHER, o); fputs(";\n", o);
                indent(o, ind + 1);
                fprintf(o, "if (_lst%d.k != AV_LIST) { fprintf(stderr, \"abyss runtime error: "
                           "'for ... in' requires a range (e.g. 0..10) or a list\\n\"); exit(70); }\n", id);
                indent(o, ind + 1);
                fprintf(o, "for (int _i%d = 0; _i%d < _lst%d.u.l->n; _i%d++) {\n", id, id, id, id);
                indent(o, ind + 2);
                fprintf(o, "AV a_%s = _lst%d.u.l->items[_i%d];\n", n->as.for_stmt.var_name, id, id);
                gen_stmt(n->as.for_stmt.body, o, ind + 2);
                indent(o, ind + 1); fputs("}\n", o);
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
