/*
 * abyssc — the Abyss compiler.
 *
 * Phase 1: front-end / lexer. Reads a .aby source file and prints the
 * token stream. This is the foundation the parser will be built on.
 */
#include <stdio.h>
#include <stdlib.h>

#include "lexer.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "abyssc: could not open '%s'\n", path);
        exit(74);
    }
    fseek(f, 0L, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fprintf(stderr, "abyssc: out of memory reading '%s'\n", path);
        exit(74);
    }
    size_t read = fread(buffer, sizeof(char), (size_t)size, f);
    buffer[read] = '\0';
    fclose(f);
    return buffer;
}

static void tokenize(const char *source) {
    Lexer lexer;
    lexer_init(&lexer, source);

    int line = -1;
    for (;;) {
        Token t = lexer_next(&lexer);

        if (t.line != line) {
            printf("%4d ", t.line);
            line = t.line;
        } else {
            printf("   | ");
        }

        if (t.type == TOK_NEWLINE) {
            printf("%-10s\n", "NEWLINE");
        } else if (t.type == TOK_ERROR) {
            printf("%-10s '%.*s'\n", "ERROR", t.length, t.start);
        } else {
            printf("%-10s '%.*s'\n", token_type_name(t.type), t.length, t.start);
        }

        if (t.type == TOK_EOF) break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: abyssc <file.aby>\n");
        return 64;
    }
    char *source = read_file(argv[1]);
    tokenize(source);
    free(source);
    return 0;
}
