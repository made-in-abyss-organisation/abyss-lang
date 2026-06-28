#ifndef ABYSS_LEXER_H
#define ABYSS_LEXER_H

#include "token.h"

typedef struct {
    const char *start;    /* start of the current lexeme   */
    const char *current;  /* current scan position         */
    int line;
} Lexer;

void  lexer_init(Lexer *lexer, const char *source);
Token lexer_next(Lexer *lexer);

#endif /* ABYSS_LEXER_H */
