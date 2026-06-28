#ifndef ABYSS_INTERP_H
#define ABYSS_INTERP_H

#include "ast.h"

typedef enum {
    VAL_NIL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_STRING,
    VAL_FN, VAL_NATIVE, VAL_STRUCT_DEF, VAL_INSTANCE, VAL_RANGE
} ValueType;

typedef struct Value Value;
typedef struct Instance Instance;
typedef Value (*NativeFn)(int argc, Value *argv);

struct Value {
    ValueType type;
    union {
        int boolean;
        long long integer;
        double floating;
        char *string;
        struct { Node *decl; } fn;          /* NODE_FN_DECL     */
        NativeFn native;
        struct { Node *decl; } struct_def;  /* NODE_STRUCT      */
        Instance *instance;                 /* heap struct value */
        struct { long long start, end; } range;
    } as;
};

/* Run a parsed program: define top-level decls, then call main().
 * Returns a process exit code. */
int interpret(Node *program);

#endif /* ABYSS_INTERP_H */
