#ifndef ABYSS_TC_H
#define ABYSS_TC_H

#include "ast.h"

/* Statically type-check a parsed program.
 * Reports each error to stderr and returns the number of errors (0 = ok). */
int typecheck(Node *program);

#endif /* ABYSS_TC_H */
