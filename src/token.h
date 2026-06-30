#ifndef ABYSS_TOKEN_H
#define ABYSS_TOKEN_H

typedef enum {
    /* literals */
    TOK_INT,        /* 42            */
    TOK_FLOAT,      /* 3.14          */
    TOK_STRING,     /* "hello"       */
    TOK_IDENT,      /* myVar         */

    /* keywords */
    TOK_LET, TOK_VAR, TOK_FN, TOK_STRUCT, TOK_COMPONENT,
    TOK_STATE, TOK_RENDER, TOK_IMPORT, TOK_AS, TOK_ASYNC,
    TOK_AWAIT, TOK_RETURN, TOK_IF, TOK_ELSE, TOK_MATCH,
    TOK_FOR, TOK_WHILE, TOK_IN, TOK_TRUE, TOK_FALSE, TOK_NIL,

    /* punctuation */
    TOK_LPAREN, TOK_RPAREN,     /* ( )   */
    TOK_LBRACE, TOK_RBRACE,     /* { }   */
    TOK_LBRACKET, TOK_RBRACKET, /* [ ]   */
    TOK_COMMA, TOK_COLON, TOK_DOT, TOK_DOTDOT,  /* . .. */

    /* operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_ASSIGN,                 /* =     */
    TOK_PLUS_EQ, TOK_MINUS_EQ,  /* += -= */
    TOK_EQ, TOK_NEQ,            /* == != */
    TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_AND, TOK_OR, TOK_NOT,   /* && || ! */
    TOK_ARROW,                  /* ->    */
    TOK_QUESTION,               /* ?     */
    TOK_QDOT,                   /* ?.    */
    TOK_QQ,                     /* ??    */

    /* meta */
    TOK_NEWLINE,
    TOK_EOF,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char *start;  /* points into the source buffer */
    int length;
    int line;
} Token;

const char *token_type_name(TokenType type);

#endif /* ABYSS_TOKEN_H */
