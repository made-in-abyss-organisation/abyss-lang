#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- small utilities ---------- */

static char *copy_slice(const char *start, int length) {
    char *s = (char *)malloc((size_t)length + 1);
    memcpy(s, start, (size_t)length);
    s[length] = '\0';
    return s;
}

static char *token_text(Token t) { return copy_slice(t.start, t.length); }

static Node *new_node(NodeType type, int line) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    n->type = type;
    n->line = line;
    return n;
}

/* ---------- token stream (newlines are treated as separators) ---------- */

static void error_at(Parser *p, Token t, const char *msg) {
    p->had_error = 1;
    if (t.type == TOK_EOF) {
        fprintf(stderr, "[line %d] parse error at end: %s\n", t.line, msg);
    } else {
        fprintf(stderr, "[line %d] parse error at '%.*s': %s\n",
                t.line, t.length, t.start, msg);
    }
}

/* next significant token: newlines are separators, errors are reported */
static Token scan(Parser *p) {
    for (;;) {
        Token t = lexer_next(&p->lexer);
        if (t.type == TOK_NEWLINE) continue;
        if (t.type == TOK_ERROR) { error_at(p, t, t.start); continue; }
        return t;
    }
}

static void advance(Parser *p) {
    p->previous = p->current;
    p->current = p->peeked;
    p->peeked = scan(p);
}

static int check(Parser *p, TokenType type) { return p->current.type == type; }
static int peek_is(Parser *p, TokenType type) { return p->peeked.type == type; }

static int match_tok(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance(p);
    return 1;
}

static void consume(Parser *p, TokenType type, const char *msg) {
    if (check(p, type)) { advance(p); return; }
    error_at(p, p->current, msg);
}

/* ---------- forward declarations ---------- */

static Node *parse_declaration(Parser *p);
static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_expression(Parser *p);

/* ---------- types (captured as a printed string, not a sub-AST yet) ---------- */

static char *parse_type(Parser *p) {
    const char *begin = p->current.start;
    consume(p, TOK_IDENT, "expected a type name");
    /* optional generic args: <T, U> */
    if (match_tok(p, TOK_LT)) {
        int depth = 1;
        while (depth > 0 && !check(p, TOK_EOF)) {
            if (check(p, TOK_LT)) depth++;
            else if (check(p, TOK_GT)) depth--;
            advance(p);
        }
    }
    match_tok(p, TOK_QUESTION);  /* optional nullable marker */
    const char *end = p->current.start;
    /* trim trailing whitespace from the captured slice */
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\n' || end[-1] == '\r')) end--;
    return copy_slice(begin, (int)(end - begin));
}

/* ---------- expressions (precedence climbing) ---------- */

static const char *op_text(TokenType t) {
    switch (t) {
        case TOK_PLUS: return "+";   case TOK_MINUS: return "-";
        case TOK_STAR: return "*";   case TOK_SLASH: return "/";
        case TOK_PERCENT: return "%";
        case TOK_EQ: return "==";    case TOK_NEQ: return "!=";
        case TOK_LT: return "<";     case TOK_GT: return ">";
        case TOK_LE: return "<=";    case TOK_GE: return ">=";
        case TOK_AND: return "&&";   case TOK_OR: return "||";
        case TOK_NOT: return "!";
        case TOK_ASSIGN: return "="; case TOK_PLUS_EQ: return "+=";
        case TOK_MINUS_EQ: return "-="; case TOK_QQ: return "??";
        default: return "?";
    }
}

static Node *parse_primary(Parser *p) {
    Token t = p->current;
    switch (t.type) {
        case TOK_INT: case TOK_FLOAT: case TOK_STRING:
        case TOK_TRUE: case TOK_FALSE: case TOK_NIL: {
            Node *n = new_node(NODE_LITERAL, t.line);
            n->as.literal.kind = t.type;
            n->as.literal.text = token_text(t);
            advance(p);
            return n;
        }
        case TOK_IDENT: {
            Node *n = new_node(NODE_IDENT, t.line);
            n->as.ident.name = token_text(t);
            advance(p);
            return n;
        }
        case TOK_LPAREN: {
            advance(p);
            Node *inner = parse_expression(p);
            consume(p, TOK_RPAREN, "expected ')' after expression");
            return inner;
        }
        default:
            error_at(p, t, "expected an expression");
            advance(p);
            return new_node(NODE_LITERAL, t.line);  /* error placeholder */
    }
}

/* postfix: calls f(...), member access .x / ?.x */
static Node *parse_postfix(Parser *p) {
    Node *expr = parse_primary(p);
    for (;;) {
        if (match_tok(p, TOK_LPAREN)) {
            Node *call = new_node(NODE_CALL, p->previous.line);
            call->as.call.callee = expr;
            nodelist_init(&call->as.call.args);
            if (!check(p, TOK_RPAREN)) {
                do {
                    /* named argument `label: expr` — skip the label for now */
                    if (check(p, TOK_IDENT) && peek_is(p, TOK_COLON)) {
                        advance(p);  /* label */
                        advance(p);  /* ':'   */
                    }
                    nodelist_push(&call->as.call.args, parse_expression(p));
                } while (match_tok(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "expected ')' after arguments");
            expr = call;
        } else if (check(p, TOK_DOT) || check(p, TOK_QDOT)) {
            int safe = check(p, TOK_QDOT);
            advance(p);
            Node *get = new_node(NODE_GET, p->previous.line);
            get->as.get.object = expr;
            get->as.get.safe = safe;
            get->as.get.name = token_text(p->current);
            consume(p, TOK_IDENT, "expected property name after '.'");
            expr = get;
        } else {
            break;
        }
    }
    return expr;
}

static Node *parse_unary(Parser *p) {
    if (check(p, TOK_NOT) || check(p, TOK_MINUS)) {
        TokenType opt = p->current.type;
        int line = p->current.line;
        advance(p);
        Node *n = new_node(NODE_UNARY, line);
        n->as.unary.op = copy_slice(op_text(opt), (int)strlen(op_text(opt)));
        n->as.unary.operand = parse_unary(p);
        return n;
    }
    return parse_postfix(p);
}

/* binary precedence levels, lowest number = binds loosest */
static int binary_prec(TokenType t) {
    switch (t) {
        case TOK_OR: return 1;
        case TOK_AND: return 2;
        case TOK_EQ: case TOK_NEQ: return 3;
        case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE: return 4;
        case TOK_QQ: return 5;
        case TOK_PLUS: case TOK_MINUS: return 6;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 7;
        default: return 0;  /* not a binary operator */
    }
}

static Node *parse_binary(Parser *p, int min_prec) {
    Node *left = parse_unary(p);
    for (;;) {
        int prec = binary_prec(p->current.type);
        if (prec == 0 || prec < min_prec) break;
        TokenType opt = p->current.type;
        int line = p->current.line;
        advance(p);
        Node *right = parse_binary(p, prec + 1);  /* left-associative */
        Node *bin = new_node(NODE_BINARY, line);
        bin->as.binary.op = copy_slice(op_text(opt), (int)strlen(op_text(opt)));
        bin->as.binary.left = left;
        bin->as.binary.right = right;
        left = bin;
    }
    return left;
}

static Node *parse_assignment(Parser *p) {
    Node *left = parse_binary(p, 1);
    if (check(p, TOK_ASSIGN) || check(p, TOK_PLUS_EQ) || check(p, TOK_MINUS_EQ)) {
        TokenType opt = p->current.type;
        int line = p->current.line;
        advance(p);
        Node *value = parse_assignment(p);  /* right-associative */
        Node *n = new_node(NODE_ASSIGN, line);
        n->as.assign.op = copy_slice(op_text(opt), (int)strlen(op_text(opt)));
        n->as.assign.target = left;
        n->as.assign.value = value;
        return n;
    }
    return left;
}

static Node *parse_expression(Parser *p) { return parse_assignment(p); }

/* ---------- declarations & statements ---------- */

static Node *parse_var_decl(Parser *p, int is_mutable) {
    Node *n = new_node(NODE_VAR_DECL, p->previous.line);
    n->as.var_decl.is_mutable = is_mutable;
    n->as.var_decl.name = token_text(p->current);
    consume(p, TOK_IDENT, "expected variable name");
    n->as.var_decl.decl_type = NULL;
    if (match_tok(p, TOK_COLON)) n->as.var_decl.decl_type = parse_type(p);
    n->as.var_decl.init = NULL;
    if (match_tok(p, TOK_ASSIGN)) n->as.var_decl.init = parse_expression(p);
    return n;
}

static Node *parse_fn_decl(Parser *p, int is_async) {
    Node *n = new_node(NODE_FN_DECL, p->previous.line);
    n->as.fn_decl.is_async = is_async;
    nodelist_init(&n->as.fn_decl.params);
    n->as.fn_decl.name = token_text(p->current);
    consume(p, TOK_IDENT, "expected function name");
    consume(p, TOK_LPAREN, "expected '(' after function name");
    if (!check(p, TOK_RPAREN)) {
        do {
            Node *param = new_node(NODE_PARAM, p->current.line);
            param->as.param.name = token_text(p->current);
            consume(p, TOK_IDENT, "expected parameter name");
            consume(p, TOK_COLON, "expected ':' after parameter name");
            param->as.param.param_type = parse_type(p);
            nodelist_push(&n->as.fn_decl.params, param);
        } while (match_tok(p, TOK_COMMA));
    }
    consume(p, TOK_RPAREN, "expected ')' after parameters");
    n->as.fn_decl.ret_type = NULL;
    if (match_tok(p, TOK_ARROW)) n->as.fn_decl.ret_type = parse_type(p);

    if (match_tok(p, TOK_ASSIGN)) {            /* expression body: fn f() = expr */
        Node *body = new_node(NODE_RETURN, p->previous.line);
        body->as.ret.value = parse_expression(p);
        n->as.fn_decl.body = body;
    } else {
        n->as.fn_decl.body = parse_block(p);   /* block body */
    }
    return n;
}

static Node *parse_block(Parser *p) {
    consume(p, TOK_LBRACE, "expected '{'");
    Node *n = new_node(NODE_BLOCK, p->previous.line);
    nodelist_init(&n->as.block.statements);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        nodelist_push(&n->as.block.statements, parse_declaration(p));
    }
    consume(p, TOK_RBRACE, "expected '}' to close block");
    return n;
}

static Node *parse_if(Parser *p) {
    Node *n = new_node(NODE_IF, p->previous.line);
    n->as.if_stmt.cond = parse_expression(p);
    n->as.if_stmt.then_branch = parse_block(p);
    n->as.if_stmt.else_branch = NULL;
    if (match_tok(p, TOK_ELSE)) {
        n->as.if_stmt.else_branch = check(p, TOK_IF)
            ? (advance(p), parse_if(p))   /* else if */
            : parse_block(p);
    }
    return n;
}

/* ---------- UI tree (the signature feature) ---------- */

/* uiArgs = uiArg { "," uiArg } ;  uiArg = [ IDENT ":" ] expr  */
static void parse_ui_args(Parser *p, NodeList *out) {
    if (check(p, TOK_RPAREN)) return;
    do {
        Node *arg = new_node(NODE_UI_ARG, p->current.line);
        arg->as.ui_arg.label = NULL;
        if (check(p, TOK_IDENT) && peek_is(p, TOK_COLON)) {
            arg->as.ui_arg.label = token_text(p->current);
            advance(p);  /* label */
            advance(p);  /* ':'   */
        }
        arg->as.ui_arg.value = parse_expression(p);
        nodelist_push(out, arg);
    } while (match_tok(p, TOK_COMMA));
}

/* uiNode = IDENT [ "(" uiArgs ")" ] [ "{" {uiNode} "}" ] { modifier } */
static Node *parse_ui_node(Parser *p) {
    Node *n = new_node(NODE_UI_NODE, p->current.line);
    n->as.ui_node.name = token_text(p->current);
    consume(p, TOK_IDENT, "expected a widget name");
    nodelist_init(&n->as.ui_node.args);
    nodelist_init(&n->as.ui_node.children);
    nodelist_init(&n->as.ui_node.modifiers);

    if (match_tok(p, TOK_LPAREN)) {
        parse_ui_args(p, &n->as.ui_node.args);
        consume(p, TOK_RPAREN, "expected ')' after widget arguments");
    }
    if (check(p, TOK_LBRACE)) {
        advance(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF))
            nodelist_push(&n->as.ui_node.children, parse_ui_node(p));
        consume(p, TOK_RBRACE, "expected '}' to close widget children");
    }
    while (check(p, TOK_DOT)) {            /* chained .modifier(...) */
        advance(p);
        Node *mod = new_node(NODE_MODIFIER, p->current.line);
        mod->as.modifier.name = token_text(p->current);
        consume(p, TOK_IDENT, "expected modifier name after '.'");
        nodelist_init(&mod->as.modifier.args);
        consume(p, TOK_LPAREN, "expected '(' after modifier name");
        parse_ui_args(p, &mod->as.modifier.args);
        consume(p, TOK_RPAREN, "expected ')' after modifier arguments");
        nodelist_push(&n->as.ui_node.modifiers, mod);
    }
    return n;
}

static Node *parse_state_decl(Parser *p) {
    Node *n = new_node(NODE_STATE_DECL, p->previous.line);
    n->as.state_decl.name = token_text(p->current);
    consume(p, TOK_IDENT, "expected state variable name");
    consume(p, TOK_COLON, "expected ':' after state name");
    n->as.state_decl.decl_type = parse_type(p);
    n->as.state_decl.init = NULL;
    if (match_tok(p, TOK_ASSIGN)) n->as.state_decl.init = parse_expression(p);
    return n;
}

/* componentDecl = "component" IDENT "{" {stateDecl|fnDecl} "render" "{" uiNode "}" "}" */
static Node *parse_component(Parser *p) {
    Node *n = new_node(NODE_COMPONENT, p->previous.line);
    n->as.component.name = token_text(p->current);
    consume(p, TOK_IDENT, "expected component name");
    consume(p, TOK_LBRACE, "expected '{' after component name");
    nodelist_init(&n->as.component.members);
    n->as.component.render_body = NULL;

    while (!check(p, TOK_RENDER) && !check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (match_tok(p, TOK_STATE)) {
            nodelist_push(&n->as.component.members, parse_state_decl(p));
        } else if (match_tok(p, TOK_FN)) {
            nodelist_push(&n->as.component.members, parse_fn_decl(p, 0));
        } else if (match_tok(p, TOK_ASYNC)) {
            consume(p, TOK_FN, "expected 'fn' after 'async'");
            nodelist_push(&n->as.component.members, parse_fn_decl(p, 1));
        } else {
            error_at(p, p->current, "expected 'state', a function, or 'render'");
            advance(p);
        }
    }
    consume(p, TOK_RENDER, "expected a 'render' block in component");
    consume(p, TOK_LBRACE, "expected '{' after 'render'");
    n->as.component.render_body = parse_ui_node(p);
    consume(p, TOK_RBRACE, "expected '}' to close render block");
    consume(p, TOK_RBRACE, "expected '}' to close component");
    return n;
}

static Node *parse_statement(Parser *p) {
    if (match_tok(p, TOK_RETURN)) {
        Node *n = new_node(NODE_RETURN, p->previous.line);
        n->as.ret.value = (check(p, TOK_RBRACE) || check(p, TOK_EOF))
                              ? NULL : parse_expression(p);
        return n;
    }
    if (match_tok(p, TOK_IF)) return parse_if(p);
    if (check(p, TOK_LBRACE)) return parse_block(p);

    Node *n = new_node(NODE_EXPR_STMT, p->current.line);
    n->as.expr_stmt.expr = parse_expression(p);
    return n;
}

static Node *parse_import(Parser *p) {
    Node *n = new_node(NODE_IMPORT, p->previous.line);
    n->as.import.path = token_text(p->current);
    consume(p, TOK_STRING, "expected a module path string after 'import'");
    n->as.import.alias = NULL;
    if (match_tok(p, TOK_AS)) {
        n->as.import.alias = token_text(p->current);
        consume(p, TOK_IDENT, "expected an alias name after 'as'");
    }
    return n;
}

static Node *parse_declaration(Parser *p) {
    if (match_tok(p, TOK_IMPORT)) return parse_import(p);
    if (match_tok(p, TOK_LET))  return parse_var_decl(p, 0);
    if (match_tok(p, TOK_VAR))  return parse_var_decl(p, 1);
    if (match_tok(p, TOK_FN))   return parse_fn_decl(p, 0);
    if (match_tok(p, TOK_COMPONENT)) return parse_component(p);
    if (match_tok(p, TOK_ASYNC)) {
        consume(p, TOK_FN, "expected 'fn' after 'async'");
        return parse_fn_decl(p, 1);
    }
    return parse_statement(p);
}

/* ---------- entry point ---------- */

Node *parse(const char *source, int *had_error) {
    Parser p;
    lexer_init(&p.lexer, source);
    p.had_error = 0;
    p.current = scan(&p);   /* prime current ... */
    p.peeked = scan(&p);    /* ... and the lookahead */
    p.previous = p.current;

    Node *program = new_node(NODE_PROGRAM, 1);
    nodelist_init(&program->as.program.declarations);
    while (!check(&p, TOK_EOF)) {
        nodelist_push(&program->as.program.declarations, parse_declaration(&p));
    }

    if (had_error) *had_error = p.had_error;
    return program;
}
