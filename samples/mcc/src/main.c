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
    printf("Usage: %s [options] <input.c> [input2.c ...]\n", prog);
    printf("\nOptions:\n");
    printf("  -o <file>         Output file (default: stdout)\n");
    printf("  -std=<standard>   C language standard:\n");
    printf("                      c89, c90, c99 (ISO standards)\n");
    printf("                      gnu89, gnu99 (GNU extensions)\n");
    printf("  -arch=<arch>      Target architecture:\n");
    printf("                      x86, x86_64, s370, s370_xa, s390, zarch\n");
    printf("                      ppc32, ppc64, ppc64le, arm64, arm64_macos\n");
    printf("  -O<level>         Optimization level (0, g, 1, 2, 3)\n");
    printf("  -E                Preprocess only\n");
    printf("  -fsyntax-only     Parse and check syntax only\n");
    printf("  -dump-ast         Print AST\n");
    printf("  -dump-sema        Print semantic analysis info (symbol table)\n");
    printf("  -dump-ir          Dump ANVIL IR (for debugging)\n");
    printf("  -I<path>          Add include path\n");
    printf("  -D<name>[=value]  Define macro\n");
    printf("  -Wall             Enable all warnings\n");
    printf("  -Wextra           Enable extra warnings\n");
    printf("  -Werror           Treat warnings as errors\n");
    printf("  -v                Verbose output\n");
    printf("  --version         Print version\n");
    printf("  --help            Print this help\n");
    printf("\nMultiple input files are compiled into a single output.\n");
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
    if (strcmp(name, "arm64") == 0 || strcmp(name, "aarch64") == 0) return MCC_ARCH_ARM64;
    if (strcmp(name, "arm64_macos") == 0 || strcmp(name, "macos") == 0) return MCC_ARCH_ARM64_MACOS;
    return MCC_ARCH_COUNT; /* Invalid */
}

/* Parse a single file and return AST (used for multi-file compilation) */
static mcc_ast_node_t *parse_file(mcc_context_t *ctx, const char *filename,
                                   mcc_preprocessor_t **out_pp,
                                   mcc_parser_t **out_parser)
{
    /* Create preprocessor */
    mcc_preprocessor_t *pp = mcc_preprocessor_create(ctx);
    if (!pp) {
        mcc_fatal(ctx, "Failed to create preprocessor");
        return NULL;
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
        return NULL;
    }
    
    /* Preprocess only? */
    if (ctx->options.preprocess_only) {
        /* Output preprocessed tokens */
        FILE *out = stdout;
        if (ctx->options.output_file) {
            out = fopen(ctx->options.output_file, "a"); /* Append for multi-file */
            if (!out) {
                mcc_fatal(ctx, "Cannot open output file: %s", ctx->options.output_file);
                mcc_preprocessor_destroy(pp);
                return NULL;
            }
        }
        
        fprintf(out, "/* File: %s */\n", filename);
        for (mcc_token_t *tok = tokens; tok && tok->type != TOK_EOF; tok = tok->next) {
            if (tok->has_space) fputc(' ', out);
            fprintf(out, "%s", mcc_token_to_string(tok));
        }
        fputc('\n', out);
        
        if (ctx->options.output_file) fclose(out);
        mcc_preprocessor_destroy(pp);
        return NULL; /* Signal preprocess-only mode */
    }
    
    /* Create parser */
    mcc_parser_t *parser = mcc_parser_create(ctx, pp);
    if (!parser) {
        mcc_fatal(ctx, "Failed to create parser");
        mcc_preprocessor_destroy(pp);
        return NULL;
    }
    
    /* Parse */
    mcc_ast_node_t *ast = mcc_parser_parse(parser);
    if (mcc_has_errors(ctx)) {
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return NULL;
    }
    
    /* Return preprocessor and parser for later cleanup */
    *out_pp = pp;
    *out_parser = parser;
    
    return ast;
}

/* Compile a single file (legacy function for backward compatibility) */
static int compile_file(mcc_context_t *ctx, const char *filename)
{
    mcc_preprocessor_t *pp = NULL;
    mcc_parser_t *parser = NULL;
    
    mcc_ast_node_t *ast = parse_file(ctx, filename, &pp, &parser);
    
    /* Handle preprocess-only mode */
    if (ctx->options.preprocess_only) {
        return mcc_has_errors(ctx) ? 1 : 0;
    }
    
    if (!ast) {
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
    
    /* Dump sema? */
    if (ctx->options.emit_sema) {
        FILE *out = stdout;
        if (ctx->options.output_file) {
            out = fopen(ctx->options.output_file, "w");
        }
        mcc_sema_dump_full(sema, ast, out);
        if (ctx->options.output_file) fclose(out);
        mcc_sema_destroy(sema);
        mcc_parser_destroy(parser);
        mcc_preprocessor_destroy(pp);
        return 0;
    }
    
    /* AST Optimization */
    mcc_ast_opt_t *ast_opt = mcc_ast_opt_create(ctx);
    mcc_ast_opt_set_level(ast_opt, ctx->options.opt_level);
    mcc_ast_opt_set_sema(ast_opt, sema);
    mcc_ast_opt_set_verbose(ast_opt, ctx->options.verbose);
    mcc_ast_opt_run(ast_opt, ast);
    mcc_ast_opt_destroy(ast_opt);
    
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
    
    /* Dump IR if requested */
    if (ctx->options.dump_ir && cg->anvil_mod) {
        /* Generate IR dump filename based on output file or default */
        char ir_filename[512];
        if (ctx->options.output_file) {
            snprintf(ir_filename, sizeof(ir_filename), "%s.ir", ctx->options.output_file);
        } else {
            snprintf(ir_filename, sizeof(ir_filename), "output.ir");
        }
        FILE *ir_file = fopen(ir_filename, "w");
        if (ir_file) {
            fprintf(ir_file, "; ANVIL IR Dump\n\n");
            anvil_dump_module(ir_file, cg->anvil_mod);
            fclose(ir_file);
            fprintf(stderr, "IR dump written to: %s\n", ir_filename);
        } else {
            fprintf(stderr, "Warning: Could not write IR dump to %s\n", ir_filename);
        }
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

/* Compile multiple files into a single output */
static int compile_files(mcc_context_t *ctx, const char **files, size_t num_files)
{
    if (num_files == 0) {
        mcc_fatal(ctx, "No input files");
        return 1;
    }
    
    /* For single file, use the simpler path */
    if (num_files == 1) {
        return compile_file(ctx, files[0]);
    }
    
    /* Arrays to hold parsed data from each file */
    mcc_ast_node_t **asts = calloc(num_files, sizeof(mcc_ast_node_t*));
    mcc_preprocessor_t **pps = calloc(num_files, sizeof(mcc_preprocessor_t*));
    mcc_parser_t **parsers = calloc(num_files, sizeof(mcc_parser_t*));
    
    if (!asts || !pps || !parsers) {
        mcc_fatal(ctx, "Out of memory");
        free(asts);
        free(pps);
        free(parsers);
        return 1;
    }
    
    /* Parse all files */
    for (size_t i = 0; i < num_files; i++) {
        if (ctx->options.verbose) {
            fprintf(stderr, "Parsing: %s\n", files[i]);
        }
        
        asts[i] = parse_file(ctx, files[i], &pps[i], &parsers[i]);
        
        /* Handle preprocess-only mode */
        if (ctx->options.preprocess_only) {
            continue;
        }
        
        if (!asts[i]) {
            /* Cleanup on error */
            for (size_t j = 0; j < i; j++) {
                if (parsers[j]) mcc_parser_destroy(parsers[j]);
                if (pps[j]) mcc_preprocessor_destroy(pps[j]);
            }
            free(asts);
            free(pps);
            free(parsers);
            return 1;
        }
    }
    
    /* Handle preprocess-only mode */
    if (ctx->options.preprocess_only) {
        free(asts);
        free(pps);
        free(parsers);
        return mcc_has_errors(ctx) ? 1 : 0;
    }
    
    /* Dump AST? */
    if (ctx->options.emit_ast) {
        FILE *out = stdout;
        if (ctx->options.output_file) {
            out = fopen(ctx->options.output_file, "w");
        }
        for (size_t i = 0; i < num_files; i++) {
            fprintf(out, "/* File: %s */\n", files[i]);
            mcc_ast_dump(asts[i], out);
            fprintf(out, "\n");
        }
        if (ctx->options.output_file) fclose(out);
        
        for (size_t i = 0; i < num_files; i++) {
            if (parsers[i]) mcc_parser_destroy(parsers[i]);
            if (pps[i]) mcc_preprocessor_destroy(pps[i]);
        }
        free(asts);
        free(pps);
        free(parsers);
        return 0;
    }
    
    /* Syntax only? */
    if (ctx->options.syntax_only) {
        printf("Syntax OK (%zu files)\n", num_files);
        for (size_t i = 0; i < num_files; i++) {
            if (parsers[i]) mcc_parser_destroy(parsers[i]);
            if (pps[i]) mcc_preprocessor_destroy(pps[i]);
        }
        free(asts);
        free(pps);
        free(parsers);
        return 0;
    }
    
    /* Create shared semantic analyzer */
    mcc_sema_t *sema = mcc_sema_create(ctx);
    
    /* Analyze all files with shared symbol table */
    for (size_t i = 0; i < num_files; i++) {
        if (ctx->options.verbose) {
            fprintf(stderr, "Analyzing: %s\n", files[i]);
        }
        
        if (!mcc_sema_analyze(sema, asts[i])) {
            mcc_sema_destroy(sema);
            for (size_t j = 0; j < num_files; j++) {
                if (parsers[j]) mcc_parser_destroy(parsers[j]);
                if (pps[j]) mcc_preprocessor_destroy(pps[j]);
            }
            free(asts);
            free(pps);
            free(parsers);
            return 1;
        }
    }
    
    /* Dump sema? */
    if (ctx->options.emit_sema) {
        FILE *out = stdout;
        if (ctx->options.output_file) {
            out = fopen(ctx->options.output_file, "w");
        }
        fprintf(out, "/* Semantic analysis for %zu files */\n\n", num_files);
        /* Dump each AST with full details */
        for (size_t i = 0; i < num_files; i++) {
            fprintf(out, "=== File: %s ===\n\n", files[i]);
            mcc_sema_dump_full(sema, asts[i], out);
        }
        if (ctx->options.output_file) fclose(out);
        
        mcc_sema_destroy(sema);
        for (size_t i = 0; i < num_files; i++) {
            if (parsers[i]) mcc_parser_destroy(parsers[i]);
            if (pps[i]) mcc_preprocessor_destroy(pps[i]);
        }
        free(asts);
        free(pps);
        free(parsers);
        return 0;
    }
    
    /* AST Optimization - optimize all ASTs */
    mcc_ast_opt_t *ast_opt = mcc_ast_opt_create(ctx);
    mcc_ast_opt_set_level(ast_opt, ctx->options.opt_level);
    mcc_ast_opt_set_sema(ast_opt, sema);
    mcc_ast_opt_set_verbose(ast_opt, ctx->options.verbose);
    
    for (size_t i = 0; i < num_files; i++) {
        if (ctx->options.verbose) {
            fprintf(stderr, "Optimizing: %s\n", files[i]);
        }
        mcc_ast_opt_run(ast_opt, asts[i]);
    }
    mcc_ast_opt_destroy(ast_opt);
    
    /* Code generation - use shared symtab and types from sema */
    mcc_codegen_t *cg = mcc_codegen_create(ctx, sema->symtab, sema->types);
    mcc_codegen_set_target(cg, ctx->options.arch);
    mcc_codegen_set_opt_level(cg, ctx->options.opt_level);
    
    /* Add all ASTs to the same module */
    for (size_t i = 0; i < num_files; i++) {
        if (ctx->options.verbose) {
            fprintf(stderr, "Generating code: %s\n", files[i]);
        }
        
        if (!mcc_codegen_add_ast(cg, asts[i])) {
            mcc_codegen_destroy(cg);
            mcc_sema_destroy(sema);
            for (size_t j = 0; j < num_files; j++) {
                if (parsers[j]) mcc_parser_destroy(parsers[j]);
                if (pps[j]) mcc_preprocessor_destroy(pps[j]);
            }
            free(asts);
            free(pps);
            free(parsers);
            return 1;
        }
    }
    
    /* Finalize code generation */
    if (!mcc_codegen_finalize(cg)) {
        mcc_codegen_destroy(cg);
        mcc_sema_destroy(sema);
        for (size_t i = 0; i < num_files; i++) {
            if (parsers[i]) mcc_parser_destroy(parsers[i]);
            if (pps[i]) mcc_preprocessor_destroy(pps[i]);
        }
        free(asts);
        free(pps);
        free(parsers);
        return 1;
    }
    
    /* Dump IR if requested */
    if (ctx->options.dump_ir && cg->anvil_mod) {
        /* Generate IR dump filename based on output file or default */
        char ir_filename[512];
        if (ctx->options.output_file) {
            snprintf(ir_filename, sizeof(ir_filename), "%s.ir", ctx->options.output_file);
        } else {
            snprintf(ir_filename, sizeof(ir_filename), "output.ir");
        }
        FILE *ir_file = fopen(ir_filename, "w");
        if (ir_file) {
            fprintf(ir_file, "; ANVIL IR Dump\n");
            fprintf(ir_file, "; Source files: %zu\n\n", num_files);
            anvil_dump_module(ir_file, cg->anvil_mod);
            fclose(ir_file);
            fprintf(stderr, "IR dump written to: %s\n", ir_filename);
        } else {
            fprintf(stderr, "Warning: Could not write IR dump to %s\n", ir_filename);
        }
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
            for (size_t i = 0; i < num_files; i++) {
                if (parsers[i]) mcc_parser_destroy(parsers[i]);
                if (pps[i]) mcc_preprocessor_destroy(pps[i]);
            }
            free(asts);
            free(pps);
            free(parsers);
            return 1;
        }
    }
    
    fwrite(output, 1, output_len, out);
    
    if (ctx->options.output_file) fclose(out);
    
    /* Cleanup */
    free(output);
    mcc_codegen_destroy(cg);
    mcc_sema_destroy(sema);
    for (size_t i = 0; i < num_files; i++) {
        if (parsers[i]) mcc_parser_destroy(parsers[i]);
        if (pps[i]) mcc_preprocessor_destroy(pps[i]);
    }
    free(asts);
    free(pps);
    free(parsers);
    
    return 0;
}

int main(int argc, char **argv)
{
    mcc_options_t opts = {0};
    opts.arch = MCC_ARCH_X86_64;  /* Default */
    opts.opt_level = MCC_OPT_NONE;
    opts.c_std = MCC_STD_DEFAULT; /* Default to C89 */
    
    /* Dynamic arrays for include paths and defines */
    const char **include_paths = NULL;
    size_t num_include_paths = 0;
    size_t cap_include_paths = 0;
    
    const char **defines = NULL;
    size_t num_defines = 0;
    size_t cap_defines = 0;
    
    /* Multiple input files support */
    const char **input_files = NULL;
    size_t num_input_files = 0;
    size_t cap_input_files = 0;
    
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
        
        if (strncmp(arg, "-std=", 5) == 0) {
            opts.c_std = mcc_c_std_from_name(arg + 5);
            if (opts.c_std == MCC_STD_DEFAULT && strcmp(arg + 5, "default") != 0) {
                fprintf(stderr, "Error: Unknown C standard: %s\n", arg + 5);
                fprintf(stderr, "Valid standards: c89, c90, c99, gnu89, gnu99\n");
                return 1;
            }
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
            char c = arg[2];
            if (c == 'g') {
                opts.opt_level = MCC_OPT_DEBUG;
            } else if (c >= '0' && c <= '3') {
                /* Map 0->NONE, 1->BASIC, 2->STANDARD, 3->AGGRESSIVE */
                static const mcc_opt_level_t levels[] = {
                    MCC_OPT_NONE, MCC_OPT_BASIC, MCC_OPT_STANDARD, MCC_OPT_AGGRESSIVE
                };
                opts.opt_level = levels[c - '0'];
            } else {
                fprintf(stderr, "Error: Invalid optimization level: %s\n", arg);
                fprintf(stderr, "Valid levels: -O0, -Og, -O1, -O2, -O3\n");
                return 1;
            }
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
        
        if (strcmp(arg, "-dump-ast") == 0) {
            opts.emit_ast = true;
            continue;
        }
        
        if (strcmp(arg, "-dump-sema") == 0) {
            opts.emit_sema = true;
            continue;
        }
        
        if (strcmp(arg, "-dump-ir") == 0) {
            opts.dump_ir = true;
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
        
        /* Input file - add to list */
        if (num_input_files >= cap_input_files) {
            cap_input_files = cap_input_files ? cap_input_files * 2 : 8;
            input_files = realloc(input_files, cap_input_files * sizeof(char*));
        }
        input_files[num_input_files++] = arg;
    }
    
    if (num_input_files == 0) {
        fprintf(stderr, "Error: No input file\n");
        print_usage(argv[0]);
        return 1;
    }
    
    opts.include_paths = include_paths;
    opts.num_include_paths = num_include_paths;
    opts.defines = defines;
    opts.num_defines = num_defines;
    opts.input_files = input_files;
    opts.num_input_files = num_input_files;
    
    /* Create context */
    mcc_context_t *ctx = mcc_context_create();
    if (!ctx) {
        fprintf(stderr, "Error: Failed to create compiler context\n");
        return 1;
    }
    
    mcc_context_set_options(ctx, &opts);
    
    /* Print configuration in verbose mode */
    if (opts.verbose) {
        const mcc_c_std_info_t *std_info = mcc_c_std_get_info(ctx->effective_std);
        fprintf(stderr, "MCC version %s\n", MCC_VERSION_STRING);
        fprintf(stderr, "C standard: %s", std_info ? std_info->name : "unknown");
        if (std_info && std_info->iso_name) {
            fprintf(stderr, " (%s)", std_info->iso_name);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "Target: %s\n", mcc_arch_name(opts.arch));
        if (opts.opt_level == MCC_OPT_DEBUG) {
            fprintf(stderr, "Optimization: Og (debug)\n");
        } else {
            /* Map back: NONE=0, BASIC=1, STANDARD=2, AGGRESSIVE=3 */
            static const int level_nums[] = {0, -1, 1, 2, 3}; /* -1 for DEBUG placeholder */
            int lvl = (opts.opt_level <= MCC_OPT_AGGRESSIVE) ? level_nums[opts.opt_level] : 0;
            fprintf(stderr, "Optimization: O%d\n", lvl);
        }
    }
    
    /* Compile all files */
    int result = compile_files(ctx, input_files, num_input_files);
    
    /* Print diagnostics summary */
    if (ctx->error_count > 0 || ctx->warning_count > 0) {
        fprintf(stderr, "%d error(s), %d warning(s)\n",
                ctx->error_count, ctx->warning_count);
    }
    
    /* Cleanup */
    mcc_context_destroy(ctx);
    free(include_paths);
    free(defines);
    free(input_files);
    
    return result;
}
