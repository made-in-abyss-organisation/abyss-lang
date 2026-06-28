#include "lexer.h"

#include <string.h>

void lexer_init(Lexer *lexer, const char *source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
}

static int is_at_end(Lexer *l)   { return *l->current == '\0'; }
static char advance(Lexer *l)    { return *l->current++; }
static char peek(Lexer *l)       { return *l->current; }
static char peek_next(Lexer *l)  { return is_at_end(l) ? '\0' : l->current[1]; }

static int match(Lexer *l, char expected) {
    if (is_at_end(l) || *l->current != expected) return 0;
    l->current++;
    return 1;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static Token make_token(Lexer *l, TokenType type) {
    Token t;
    t.type = type;
    t.start = l->start;
    t.length = (int)(l->current - l->start);
    t.line = l->line;
    return t;
}

static Token error_token(Lexer *l, const char *msg) {
    Token t;
    t.type = TOK_ERROR;
    t.start = msg;
    t.length = (int)strlen(msg);
    t.line = l->line;
    return t;
}

/* Skip spaces/tabs and // comments, but NOT newlines (they are tokens). */
static void skip_trivia(Lexer *l) {
    for (;;) {
        char c = peek(l);
        if (c == ' ' || c == '\r' || c == '\t') {
            advance(l);
        } else if (c == '/' && peek_next(l) == '/') {
            while (peek(l) != '\n' && !is_at_end(l)) advance(l);
        } else {
            return;
        }
    }
}

static TokenType keyword_or_ident(const char *s, int len) {
    struct { const char *kw; TokenType type; } table[] = {
        {"let", TOK_LET}, {"var", TOK_VAR}, {"fn", TOK_FN},
        {"struct", TOK_STRUCT}, {"component", TOK_COMPONENT},
        {"state", TOK_STATE}, {"render", TOK_RENDER},
        {"import", TOK_IMPORT}, {"as", TOK_AS}, {"async", TOK_ASYNC},
        {"await", TOK_AWAIT}, {"return", TOK_RETURN}, {"if", TOK_IF},
        {"else", TOK_ELSE}, {"match", TOK_MATCH}, {"for", TOK_FOR},
        {"in", TOK_IN}, {"true", TOK_TRUE}, {"false", TOK_FALSE},
        {"nil", TOK_NIL},
    };
    int n = (int)(sizeof(table) / sizeof(table[0]));
    for (int i = 0; i < n; i++) {
        if ((int)strlen(table[i].kw) == len &&
            memcmp(s, table[i].kw, len) == 0) {
            return table[i].type;
        }
    }
    return TOK_IDENT;
}

static Token identifier(Lexer *l) {
    while (is_alpha(peek(l)) || is_digit(peek(l))) advance(l);
    int len = (int)(l->current - l->start);
    return make_token(l, keyword_or_ident(l->start, len));
}

static Token number(Lexer *l) {
    while (is_digit(peek(l))) advance(l);
    if (peek(l) == '.' && is_digit(peek_next(l))) {
        advance(l); /* consume '.' */
        while (is_digit(peek(l))) advance(l);
        return make_token(l, TOK_FLOAT);
    }
    return make_token(l, TOK_INT);
}

static Token string(Lexer *l) {
    while (peek(l) != '"' && !is_at_end(l)) {
        if (peek(l) == '\n') l->line++;
        advance(l);
    }
    if (is_at_end(l)) return error_token(l, "unterminated string");
    advance(l); /* closing quote */
    return make_token(l, TOK_STRING);
}

Token lexer_next(Lexer *l) {
    skip_trivia(l);
    l->start = l->current;

    if (is_at_end(l)) return make_token(l, TOK_EOF);

    char c = advance(l);

    if (c == '\n') { Token t = make_token(l, TOK_NEWLINE); l->line++; return t; }
    if (is_alpha(c)) return identifier(l);
    if (is_digit(c)) return number(l);

    switch (c) {
        case '(': return make_token(l, TOK_LPAREN);
        case ')': return make_token(l, TOK_RPAREN);
        case '{': return make_token(l, TOK_LBRACE);
        case '}': return make_token(l, TOK_RBRACE);
        case ',': return make_token(l, TOK_COMMA);
        case ':': return make_token(l, TOK_COLON);
        case '.': return make_token(l, TOK_DOT);
        case '*': return make_token(l, TOK_STAR);
        case '/': return make_token(l, TOK_SLASH);
        case '%': return make_token(l, TOK_PERCENT);
        case '"': return string(l);
        case '+': return make_token(l, match(l, '=') ? TOK_PLUS_EQ : TOK_PLUS);
        case '-': return make_token(l, match(l, '>') ? TOK_ARROW
                                     : match(l, '=') ? TOK_MINUS_EQ : TOK_MINUS);
        case '=': return make_token(l, match(l, '=') ? TOK_EQ : TOK_ASSIGN);
        case '!': return make_token(l, match(l, '=') ? TOK_NEQ : TOK_NOT);
        case '<': return make_token(l, match(l, '=') ? TOK_LE : TOK_LT);
        case '>': return make_token(l, match(l, '=') ? TOK_GE : TOK_GT);
        case '&': if (match(l, '&')) return make_token(l, TOK_AND); break;
        case '|': if (match(l, '|')) return make_token(l, TOK_OR); break;
        case '?': if (match(l, '.')) return make_token(l, TOK_QDOT);
                  if (match(l, '?')) return make_token(l, TOK_QQ);
                  return make_token(l, TOK_QUESTION);
    }

    return error_token(l, "unexpected character");
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_INT: return "INT";
        case TOK_FLOAT: return "FLOAT";
        case TOK_STRING: return "STRING";
        case TOK_IDENT: return "IDENT";
        case TOK_LET: return "LET";
        case TOK_VAR: return "VAR";
        case TOK_FN: return "FN";
        case TOK_STRUCT: return "STRUCT";
        case TOK_COMPONENT: return "COMPONENT";
        case TOK_STATE: return "STATE";
        case TOK_RENDER: return "RENDER";
        case TOK_IMPORT: return "IMPORT";
        case TOK_AS: return "AS";
        case TOK_ASYNC: return "ASYNC";
        case TOK_AWAIT: return "AWAIT";
        case TOK_RETURN: return "RETURN";
        case TOK_IF: return "IF";
        case TOK_ELSE: return "ELSE";
        case TOK_MATCH: return "MATCH";
        case TOK_FOR: return "FOR";
        case TOK_IN: return "IN";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NIL: return "NIL";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_COMMA: return "COMMA";
        case TOK_COLON: return "COLON";
        case TOK_DOT: return "DOT";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_ASSIGN: return "ASSIGN";
        case TOK_PLUS_EQ: return "PLUS_EQ";
        case TOK_MINUS_EQ: return "MINUS_EQ";
        case TOK_EQ: return "EQ";
        case TOK_NEQ: return "NEQ";
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_LE: return "LE";
        case TOK_GE: return "GE";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_NOT: return "NOT";
        case TOK_ARROW: return "ARROW";
        case TOK_QUESTION: return "QUESTION";
        case TOK_QDOT: return "QDOT";
        case TOK_QQ: return "QQ";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}
