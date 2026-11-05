/* anvil-bench-gen KERNEL OUT.s [--arch=NAME] [--cpu=NAME] [-O0|-O1|-O2|-O3]
 * Builds one benchmark kernel with the ANVIL API and writes its assembly
 * for a Linux (System V) target. */
#include "kit.h"
#include "anvil/anvil_opt.h"
#include "anvil/anvil_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    anvil_arch_t arch;
} arches[] = {
    {"x86_64", ANVIL_ARCH_X86_64}, {"x86", ANVIL_ARCH_X86},     {"arm64", ANVIL_ARCH_ARM64},
    {"ppc64", ANVIL_ARCH_PPC64},   {"ppc64le", ANVIL_ARCH_PPC64LE}, {"ppc32", ANVIL_ARCH_PPC32},
    /* HLASM output: for reading (run.sh does not assemble it). */
    {"zarch", ANVIL_ARCH_ZARCH},   {"s390", ANVIL_ARCH_S390},       {"s370", ANVIL_ARCH_S370},
};

static int usage(void)
{
    fprintf(stderr, "usage: anvil-bench-gen KERNEL OUT.s [--arch=NAME] [--cpu=NAME] [-O0|-O1|-O2|-O3] [--ir]\n--ir prints the optimized IR to stderr\nkernels:");
    for (size_t i = 0; i < bench_num_kernels; i++)
        fprintf(stderr, " %s", bench_kernels[i].name);
    fprintf(stderr, "\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    const bench_kernel_t *kernel = NULL;
    for (size_t i = 0; i < bench_num_kernels; i++)
        if (strcmp(argv[1], bench_kernels[i].name) == 0)
            kernel = &bench_kernels[i];
    if (!kernel)
        return usage();

    anvil_arch_t arch = ANVIL_ARCH_X86_64;
    const char *cpu_name = NULL;
    bool dump_ir = false;
    anvil_opt_level_t level = ANVIL_OPT_STANDARD;
    for (int i = 3; i < argc; i++) {
        bool known = false;
        for (size_t a = 0; a < sizeof(arches) / sizeof(arches[0]); a++)
            if (strncmp(argv[i], "--arch=", 7) == 0 && strcmp(argv[i] + 7, arches[a].name) == 0) {
                arch = arches[a].arch;
                known = true;
            }
        if (strcmp(argv[i], "--ir") == 0) {
            dump_ir = true;
            known = true;
        }
        if (strncmp(argv[i], "--cpu=", 6) == 0) {
            cpu_name = argv[i] + 6;
            known = true;
        }
        if (argv[i][0] == '-' && argv[i][1] == 'O' && argv[i][2] >= '0' && argv[i][2] <= '3' && !argv[i][3]) {
            static const anvil_opt_level_t levels[] = {ANVIL_OPT_NONE, ANVIL_OPT_BASIC, ANVIL_OPT_STANDARD, ANVIL_OPT_AGGRESSIVE};
            level = levels[argv[i][2] - '0'];
            known = true;
        }
        if (!known)
            return usage();
    }

    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_cpu_model_t cpu;
    /* SysV where the target has it (the C references' convention), the
     * target's own ABI otherwise. */
    if (ctx && anvil_ctx_set_target(ctx, arch) == ANVIL_OK)
        (void)anvil_ctx_set_abi(ctx, ANVIL_ABI_SYSV);
    if (!ctx || anvil_ctx_get_arch_info(ctx) == NULL) {
        fprintf(stderr, "cannot set up the target\n");
        return 1;
    }
    if (cpu_name && (!anvil_cpu_model_lookup(arch, cpu_name, &cpu) || anvil_ctx_set_cpu(ctx, cpu) != ANVIL_OK)) {
        fprintf(stderr, "unknown CPU model '%s'\n", cpu_name);
        return 1;
    }
    anvil_ctx_set_opt_level(ctx, level);

    bench_t b = {.ctx = ctx};
    b.mod = anvil_module_create(ctx, kernel->name);
    b.void_type = anvil_type_void(ctx);
    b.i1 = anvil_type_i1(ctx);
    b.i8 = anvil_type_i8(ctx);
    b.u8 = anvil_type_u8(ctx);
    b.i32 = anvil_type_i32(ctx);
    b.u32 = anvil_type_u32(ctx);
    b.i64 = anvil_type_i64(ctx);
    b.f64 = anvil_type_f64(ctx);
    const anvil_arch_info_t *info = anvil_ctx_get_arch_info(ctx);
    bool wide = info && info->ptr_size == 8;
    b.index = wide ? b.i64 : b.i32;
    b.uindex = wide ? anvil_type_u64(ctx) : b.u32;

    char *text = NULL;
    size_t len = 0;
    bool built = b.mod && kernel->build(&b);
    if (built && dump_ir && anvil_module_optimize(b.mod) == ANVIL_OK)
        anvil_dump_module(stderr, b.mod);
    if (!built || anvil_module_codegen(b.mod, &text, &len) != ANVIL_OK || !text) {
        fprintf(stderr, "%s: %s\n", kernel->name, anvil_ctx_get_error(ctx));
        return 1;
    }
    FILE *out = fopen(argv[2], "w");
    if (!out || fwrite(text, 1, len, out) != len || fclose(out) != 0) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    free(text);
    anvil_module_destroy(b.mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
