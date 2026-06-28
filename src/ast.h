#ifndef ABYSS_AST_H
#define ABYSS_AST_H

#include "token.h"

typedef enum {
    /* expressions */
    NODE_LITERAL,    /* int/float/string/bool/nil          */
    NODE_IDENT,      /* a name                             */
    NODE_UNARY,      /* op operand        (! -)            */
    NODE_BINARY,     /* left op right                      */
    NODE_ASSIGN,     /* target op value   (= += -=)        */
    NODE_CALL,       /* callee(args...)                    */
    NODE_GET,        /* object.name  or  object?.name      */
    /* declarations / statements */
    NODE_VAR_DECL,   /* let/var name: type = init          */
    NODE_FN_DECL,    /* fn name(params) -> ret { body }     */
    NODE_PARAM,      /* name: type                         */
    NODE_RETURN,     /* return value?                      */
    NODE_IF,         /* if cond { then } else { else }      */
    NODE_BLOCK,      /* { statements }                     */
    NODE_EXPR_STMT,  /* an expression used as a statement   */
    NODE_PROGRAM     /* the whole file                     */
} NodeType;

typedef struct Node Node;

/* a growable list of node pointers */
typedef struct {
    Node **items;
    int count;
    int capacity;
} NodeList;

struct Node {
    NodeType type;
    int line;
    union {
        struct { TokenType kind; char *text; } literal;
        struct { char *name; } ident;
        struct { char *op; Node *operand; } unary;
        struct { char *op; Node *left; Node *right; } binary;
        struct { char *op; Node *target; Node *value; } assign;
        struct { Node *callee; NodeList args; } call;
        struct { Node *object; char *name; int safe; } get;  /* safe = ?. */
        struct { int is_mutable; char *name; char *decl_type; Node *init; } var_decl;
        struct { int is_async; char *name; NodeList params;
                 char *ret_type; Node *body; } fn_decl;
        struct { char *name; char *param_type; } param;
        struct { Node *value; } ret;                          /* value may be NULL */
        struct { Node *cond; Node *then_branch; Node *else_branch; } if_stmt;
        struct { NodeList statements; } block;
        struct { Node *expr; } expr_stmt;
        struct { NodeList declarations; } program;
    } as;
};

/* list helpers */
void nodelist_init(NodeList *list);
void nodelist_push(NodeList *list, Node *node);

/* pretty-print the tree to stdout */
void ast_print(Node *node);

#endif /* ABYSS_AST_H */
