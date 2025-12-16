/*
 * MCC - Micro C Compiler
 * Main entry point
 */

#include "mcc.h"
#include <getopt.h>

static void print_version(void)
{
    printf("MCC - Micro C Compiler version %s\n", MCC_VERSION_STRING);
    printf("Using ANVIL for code generation\n");
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] <input.c>\n", prog);
    printf("\nOptions:\n");
    printf("  -o <file>         Output file (default: stdout)\n");
    printf("  -arch=<arch>      Target architecture:\n");
    printf("                      x86, x86_64, s370, s370_xa, s390, zarch\n");
    printf("                      ppc32, ppc64, ppc64le, arm64\n");
    printf("  -O<level>         Optimization level (0-3)\n");
    printf("  -E                Preprocess only\n");
    printf("  -fsyntax-only     Parse and check syntax only\n");
    printf("  -ast-dump         Print AST\n");
    printf("  -I<path>          Add include path\n");
    printf("  -D<name>[=value]  Define macro\n");
    printf("  -Wall             Enable all warnings\n");
    printf("  -Wextra           Enable extra warnings\n");
    printf("  -Werror           Treat warnings as errors\n");
    printf("  -v                Verbose output\n");
    printf("  --version         Print version\n");
    printf("  --help            Print this help\n");
}

static mcc_arch_t parse_arch(const char *name)
{
    if (strcmp(name, "x86") == 0) return MCC_ARCH_X86;
    if (strcmp(name, "x86_64") == 0 || strcmp(name, "x64") == 0) return MCC_ARCH_X86_64;
    if (strcmp(name, "s370") == 0) return MCC_ARCH_S370;
    if (strcmp(name, "s370_xa") == 0 || strcmp(name, "s370xa") == 0) return MCC_ARCH_S370_XA;
    if (strcmp(name, "s390") == 0) return MCC_ARCH_S390;
    if (strcmp(name, "zarch") == 0 || strcmp(name, "z") == 0) return MCC_ARCH_ZARCH;
    if (strcmp(name, "ppc32") == 0 || strcmp(name, "ppc") == 0) return MCC_ARCH_PPC32;
    if (strcmp(name, "ppc64") == 0) return MCC_ARCH_PPC64;
    if (strcmp(name, "ppc64le") == 0) return MCC_ARCH_PPC64LE;
    if (strcmp(name, "arm64") == 0) return MCC_ARCH_ARM64;
    return MCC_ARCH_COUNT; /* Invalid */
}

static int compile_file(mcc_context_t *ctx, const char *filename)
{
    /* Create preprocessor */
    mcc_preprocessor_t *pp = mcc_preprocessor_create(ctx);
    if (!pp) {
        mcc_fatal(ctx, "Failed to create preprocessor");
        return 1;
    }
    
    /* Add include paths */
    for (size_t i = 0; i < ctx->options.num_include_paths; i++) {
        mcc_preprocessor_add_include_path(pp, ctx->options.include_paths[i]);
    }
    
    /* Add defines */
    for (size_t i = 0; i < ctx->options.num_defines; i++) {
        const char *def = ctx->options.defines[i];
        const char *eq = strchr(def, '=');
        if (eq) {
            char *name = mcc_alloc(ctx, eq - def + 1);
            memcpy(name, def, eq - def);
            name[eq - def] = '\0';
            mcc_preprocessor_define(pp, name, eq + 1);
        } else {
            mcc_preprocessor_define(pp, def, "1");
        }
    }
    
    /* Define built-in macros */
    mcc_preprocessor_define_builtins(pp);
    
    /* Run preprocessor */
    mcc_token_t *tokens = mcc_preprocessor_run(pp, filename);
    if (mcc_has_errors(ctx)) {
        mcc_preprocessor_destroy(pp);
        return 1;
    }
    
    /* Preprocess only? */
    if (ctx->options.preprocess_only) {
        /* Output preprocessed tokens */
        FILE *out = stdout;
        if (ctx->options.output_file) {
            out = fopen(ctx->options.output_file, "w");
            if (!out) {
                mcc_fatal(ctx, "Cannot open output file: %s", ctx->options.output_file);
                mcc_preprocessor_destroy(pp);
                return 1;
            }
        }
        
        for (mcc_token_t *tok = tokens; tok && tok->type != TOK_EOF; tok = tok->next) {
            if (tok->has_space) fputc(' ', out);
            fprintf(out, "%s", mcc_token_to_string(tok));
        }
        fputc('\n', out);
        
        if (ctx->options.output_file) fclose(out);
        mcc_preprocessor_destroy(pp);
        return 0;
    }
    
    /* Create parser */
    mcc_parser_t *parser = mcc_parser_create(ctx, pp);
    if (!parser) {
        mcc_fatal(ctx, "Failed to create parser");
        mcc_preprocessor_destroy(pp);
        return 1;
    }
    
    /* Parse */
    mcc_ast_node_t *ast = mcc_parser_parse(parser);
    if (mcc_has_errors(ctx)) {
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return 1;
    }
    
    /* Dump AST? */
    if (ctx->options.emit_ast) {
        FILE *out = stdout;
        if (ctx->options.output_file) {
            out = fopen(ctx->options.output_file, "w");
        }
        mcc_ast_dump(ast, out);
        if (ctx->options.output_file) fclose(out);
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return 0;
    }
    
    /* Syntax only? */
    if (ctx->options.syntax_only) {
        printf("Syntax OK\n");
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return 0;
    }
    
    /* Semantic analysis */
    mcc_sema_t *sema = mcc_sema_create(ctx);
    if (!mcc_sema_analyze(sema, ast)) {
        mcc_sema_destroy(sema);
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return 1;
    }
    
    /* Code generation - use symtab and types from sema */
    mcc_codegen_t *cg = mcc_codegen_create(ctx, sema->symtab, sema->types);
    mcc_codegen_set_target(cg, ctx->options.arch);
    mcc_codegen_set_opt_level(cg, ctx->options.opt_level);
    
    if (!mcc_codegen_generate(cg, ast)) {
        mcc_codegen_destroy(cg);
        mcc_sema_destroy(sema);
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return 1;
    }
    
    /* Get output */
    size_t output_len;
    char *output = mcc_codegen_get_output(cg, &output_len);
    
    /* Write output */
    FILE *out = stdout;
    if (ctx->options.output_file) {
        out = fopen(ctx->options.output_file, "w");
        if (!out) {
            mcc_fatal(ctx, "Cannot open output file: %s", ctx->options.output_file);
            free(output);
            mcc_codegen_destroy(cg);
            mcc_sema_destroy(sema);
            mcc_parser_destroy(parser);
            mcc_preprocessor_destroy(pp);
            return 1;
        }
    }
    
    fwrite(output, 1, output_len, out);
    
    if (ctx->options.output_file) fclose(out);
    
    /* Cleanup */
    free(output);
    mcc_codegen_destroy(cg);
    mcc_sema_destroy(sema);
    mcc_parser_destroy(parser);
    mcc_preprocessor_destroy(pp);
    
    return 0;
}

int main(int argc, char **argv)
{
    mcc_options_t opts = {0};
    opts.arch = MCC_ARCH_X86_64;  /* Default */
    opts.opt_level = MCC_OPT_NONE;
    
    /* Dynamic arrays for include paths and defines */
    const char **include_paths = NULL;
    size_t num_include_paths = 0;
    size_t cap_include_paths = 0;
    
    const char **defines = NULL;
    size_t num_defines = 0;
    size_t cap_defines = 0;
    
    const char *input_file = NULL;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        
        if (strcmp(arg, "--version") == 0) {
            print_version();
            return 0;
        }
        
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires an argument\n");
                return 1;
            }
            opts.output_file = argv[++i];
            continue;
        }
        
        if (strncmp(arg, "-arch=", 6) == 0) {
            opts.arch = parse_arch(arg + 6);
            if (opts.arch == MCC_ARCH_COUNT) {
                fprintf(stderr, "Error: Unknown architecture: %s\n", arg + 6);
                return 1;
            }
            continue;
        }
        
        if (strncmp(arg, "-O", 2) == 0) {
            int level = arg[2] - '0';
            if (level < 0 || level > 3) {
                fprintf(stderr, "Error: Invalid optimization level: %s\n", arg);
                return 1;
            }
            opts.opt_level = (mcc_opt_level_t)level;
            continue;
        }
        
        if (strcmp(arg, "-E") == 0) {
            opts.preprocess_only = true;
            continue;
        }
        
        if (strcmp(arg, "-fsyntax-only") == 0) {
            opts.syntax_only = true;
            continue;
        }
        
        if (strcmp(arg, "-ast-dump") == 0) {
            opts.emit_ast = true;
            continue;
        }
        
        if (strncmp(arg, "-I", 2) == 0) {
            const char *path = arg[2] ? arg + 2 : argv[++i];
            if (num_include_paths >= cap_include_paths) {
                cap_include_paths = cap_include_paths ? cap_include_paths * 2 : 8;
                include_paths = realloc(include_paths, cap_include_paths * sizeof(char*));
            }
            include_paths[num_include_paths++] = path;
            continue;
        }
        
        if (strncmp(arg, "-D", 2) == 0) {
            const char *def = arg[2] ? arg + 2 : argv[++i];
            if (num_defines >= cap_defines) {
                cap_defines = cap_defines ? cap_defines * 2 : 8;
                defines = realloc(defines, cap_defines * sizeof(char*));
            }
            defines[num_defines++] = def;
            continue;
        }
        
        if (strcmp(arg, "-Wall") == 0) {
            opts.warn_all = true;
            continue;
        }
        
        if (strcmp(arg, "-Wextra") == 0) {
            opts.warn_extra = true;
            continue;
        }
        
        if (strcmp(arg, "-Werror") == 0) {
            opts.warn_error = true;
            continue;
        }
        
        if (strcmp(arg, "-v") == 0) {
            opts.verbose = true;
            continue;
        }
        
        if (arg[0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", arg);
            return 1;
        }
        
        /* Input file */
        if (input_file) {
            fprintf(stderr, "Error: Multiple input files not supported\n");
            return 1;
        }
        input_file = arg;
    }
    
    if (!input_file) {
        fprintf(stderr, "Error: No input file\n");
        print_usage(argv[0]);
        return 1;
    }
    
    opts.include_paths = include_paths;
    opts.num_include_paths = num_include_paths;
    opts.defines = defines;
    opts.num_defines = num_defines;
    
    /* Create context */
    mcc_context_t *ctx = mcc_context_create();
    if (!ctx) {
        fprintf(stderr, "Error: Failed to create compiler context\n");
        return 1;
    }
    
    mcc_context_set_options(ctx, &opts);
    
    /* Compile */
    int result = compile_file(ctx, input_file);
    
    /* Print diagnostics summary */
    if (ctx->error_count > 0 || ctx->warning_count > 0) {
        fprintf(stderr, "%d error(s), %d warning(s)\n",
                ctx->error_count, ctx->warning_count);
    }
    
    /* Cleanup */
    mcc_context_destroy(ctx);
    free(include_paths);
    free(defines);
    
    return result;
}
