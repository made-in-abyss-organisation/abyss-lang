/*
 * abyssc — the Abyss compiler.
 *
 * Phase 1 front-end:
 *   abyssc <file.aby>            parse and print the AST
 *   abyssc --tokens <file.aby>   print the raw token stream (lexer)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"

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

static void dump_tokens(const char *source) {
    Lexer lexer;
    lexer_init(&lexer, source);
    int line = -1;
    for (;;) {
        Token t = lexer_next(&lexer);
        if (t.line != line) { printf("%4d ", t.line); line = t.line; }
        else printf("   | ");
        if (t.type == TOK_NEWLINE)      printf("%-10s\n", "NEWLINE");
        else if (t.type == TOK_ERROR)   printf("%-10s '%.*s'\n", "ERROR", t.length, t.start);
        else printf("%-10s '%.*s'\n", token_type_name(t.type), t.length, t.start);
        if (t.type == TOK_EOF) break;
    }
}

int main(int argc, char *argv[]) {
    const char *path = NULL;
    int tokens_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) tokens_only = 1;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: abyssc [--tokens] <file.aby>\n");
        return 64;
    }

    char *source = read_file(path);

    if (tokens_only) {
        dump_tokens(source);
        free(source);
        return 0;
    }

    int had_error = 0;
    Node *program = parse(source, &had_error);
    if (had_error) {
        fprintf(stderr, "abyssc: parsing failed.\n");
        free(source);
        return 65;
    }
    ast_print(program);
    free(source);
    return 0;
}
