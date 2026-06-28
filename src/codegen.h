#ifndef ABYSS_CODEGEN_H
#define ABYSS_CODEGEN_H

#include <stdio.h>
#include "ast.h"

/* Transpile a parsed program to C, written to `out`.
 * Returns 0 on success, or the number of unsupported constructs encountered
 * (those are emitted as nil placeholders so the C still compiles). */
int emit_c(Node *program, FILE *out);

#endif /* ABYSS_CODEGEN_H */
