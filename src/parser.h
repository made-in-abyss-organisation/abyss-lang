#ifndef ABYSS_PARSER_H
#define ABYSS_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer lexer;
    Token current;
    Token previous;
    Token peeked;    /* one-token lookahead beyond `current` */
    int had_error;
} Parser;

/* Parse a whole source buffer into a NODE_PROGRAM.
 * Returns NULL only on allocation failure; sets had_error on syntax errors. */
Node *parse(const char *source, int *had_error);

#endif /* ABYSS_PARSER_H */
