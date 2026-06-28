/*
 * abyssc — the Abyss compiler / runner.
 *
 *   abyssc <file.aby>            run the program (calls main)
 *   abyssc --ast <file.aby>      parse and print the AST
 *   abyssc --tokens <file.aby>   print the raw token stream (lexer)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "tc.h"

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
    int mode_tokens = 0, mode_ast = 0, mode_emit_c = 0, no_check = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) mode_tokens = 1;
        else if (strcmp(argv[i], "--ast") == 0) mode_ast = 1;
        else if (strcmp(argv[i], "--emit-c") == 0) mode_emit_c = 1;
        else if (strcmp(argv[i], "--no-check") == 0) no_check = 1;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: abyssc [--ast|--tokens|--emit-c|--no-check] <file.aby>\n");
        return 64;
    }

    char *source = read_file(path);

    if (mode_tokens) {
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

    if (mode_ast) {
        ast_print(program);
        free(source);
        return 0;
    }

    /* --emit-c needs the type annotations the checker writes onto the AST,
     * so always type-check before codegen (even under --no-check). */
    if (!no_check || mode_emit_c) {
        int errs = typecheck(program);
        if (errs > 0) {
            fprintf(stderr, "abyssc: %d type error(s); not running.\n", errs);
            free(source);
            return 65;
        }
    }

    if (mode_emit_c) {
        int unsupported = emit_c(program, stdout);
        if (unsupported > 0)
            fprintf(stderr, "abyssc: warning: %d construct(s) not yet supported by "
                            "the C backend (emitted as nil)\n", unsupported);
        free(source);
        return 0;
    }

    int code = interpret(program);
    free(source);
    return code;
}
