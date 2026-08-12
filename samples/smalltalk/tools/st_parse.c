#include "st_parser.h"

#include <stdio.h>
#include <stdlib.h>

static int parse_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    st_ast_unit_t unit;
    st_parser_t parser;
    const st_parse_error_t *error;
    int result = EXIT_FAILURE;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (!st_ast_unit_init(&unit, path)) {
        fprintf(stderr, "%s: cannot initialize AST: %s\n", path,
                st_ast_status_string(st_ast_unit_status(&unit)));
        fclose(file);
        return EXIT_FAILURE;
    }
    if (!st_parser_init_file(&parser, &unit, file)) {
        fprintf(stderr, "%s: cannot initialize parser: %s\n", path,
                st_parse_status_string(st_parser_status(&parser)));
        st_parser_destroy(&parser);
        st_ast_unit_destroy(&unit);
        fclose(file);
        return EXIT_FAILURE;
    }
    if (st_parse_source_unit(&parser)) {
        printf("%s: %zu form(s), %zu declaration(s)\n", path,
               unit.forms.count, unit.declarations.count);
        result = EXIT_SUCCESS;
    } else {
        error = st_parser_error(&parser);
        fprintf(stderr,
                "%s:%zu:%zu: %s; got %s",
                path, error->span.begin.line, error->span.begin.column,
                st_parse_status_string(error->status),
                st_token_kind_name(error->actual));
        if (error->expected != ST_TOKEN_NONE) {
            fprintf(stderr, "; expected token mask 0x%08x",
                    (unsigned)error->expected);
        }
        fputc('\n', stderr);
    }
    st_parser_destroy(&parser);
    st_ast_unit_destroy(&unit);
    if (fclose(file) != 0) {
        perror(path);
        result = EXIT_FAILURE;
    }
    return result;
}

int main(int argc, char **argv)
{
    int failures = 0;
    int index;
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.st [FILE.st ...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    for (index = 1; index < argc; index++) {
        if (parse_file(argv[index]) != EXIT_SUCCESS) {
            failures++;
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
