#include "st_dispatch.h"

#include "anvil/anvil_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, message) do {                                         \
    if (!(condition)) {                                                        \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, (message));              \
        failures++;                                                            \
    }                                                                          \
} while (0)

static anvil_instr_t *find_indirect_call(anvil_func_t *function)
{
    if (!function) return NULL;
    for (anvil_block_t *block = function->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_CALL && instr->num_operands > 0 &&
                instr->operands[0] &&
                instr->operands[0]->kind == ANVIL_VAL_INSTR) {
                return instr;
            }
        }
    }
    return NULL;
}

static anvil_instr_t *find_direct_call(anvil_func_t *function,
                                       const char *callee_name)
{
    if (!function || !callee_name) return NULL;
    for (anvil_block_t *block = function->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_CALL && instr->num_operands > 0 &&
                instr->operands[0] &&
                instr->operands[0]->kind == ANVIL_VAL_FUNC &&
                instr->operands[0]->name &&
                strcmp(instr->operands[0]->name, callee_name) == 0) {
                return instr;
            }
        }
    }
    return NULL;
}

static bool write_text_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

static void run_x64_dispatch(const char *assembly)
{
#if defined(__x86_64__) && !defined(_WIN32)
    long pid = (long)getpid();
    char asm_path[128], harness_path[128], executable_path[128], command[768];
    snprintf(asm_path, sizeof(asm_path),
             "/tmp/anvil-smalltalk-dispatch-%ld.s", pid);
    snprintf(harness_path, sizeof(harness_path),
             "/tmp/anvil-smalltalk-dispatch-%ld.c", pid);
    snprintf(executable_path, sizeof(executable_path),
             "/tmp/anvil-smalltalk-dispatch-%ld", pid);
    const char *harness =
        "#include \"st_dispatch.h\"\n"
        "extern uint64_t st_dispatch(StFrame *, uint32_t);\n"
        "static unsigned misses;\n"
        "uint64_t st_dispatch_miss(StFrame *frame, uint32_t slot) {\n"
        "  ++misses; return frame->receiver + 1000 + slot + frame->root_count;\n"
        "}\n"
        "int main(void) {\n"
        "  uint64_t args[2] = { 7, 99 };\n"
        "  uint64_t roots[2] = { 10, 7 };\n"
        "  StFrame caller = { 0 };\n"
        "  StFrame frame = { .thread = (void *)0x1234, .caller = &caller,\n"
        "    .method = (const StMethodDescriptor *)0x2000,\n"
        "    .home = (StHomeToken *)0x3000, .receiver = 10, .argv = args,\n"
        "    .roots = roots, .argc = 2, .root_count = 2,\n"
        "    .safepoint_id = 17, .flags = 5 };\n"
        "  if (st_dispatch(&frame, 0) != 12) return 1;\n"
        "  if (st_dispatch(&frame, 1) != 17) return 2;\n"
        "  frame.argc = 0; frame.argv = 0;\n"
        "  if (st_dispatch(&frame, 1) != 10) return 6;\n"
        "  frame.receiver = 100; frame.argc = 1; args[0] = 23;\n"
        "  frame.argv = args;\n"
        "  if (st_dispatch(&frame, 2) != 1104) return 3;\n"
        "  if (st_dispatch(&frame, 3) != 1105) return 4;\n"
        "  if (misses != 2 || frame.caller != &caller || frame.roots != roots ||\n"
        "      frame.method != (const StMethodDescriptor *)0x2000 ||\n"
        "      frame.home != (StHomeToken *)0x3000 ||\n"
        "      frame.safepoint_id != 17 || frame.flags != 5) return 5;\n"
        "  return 0;\n"
        "}\n";
    bool wrote = write_text_file(asm_path, assembly) &&
                 write_text_file(harness_path, harness);
    CHECK(wrote, "x64 dispatch test should create its temporary sources");
    if (wrote) {
        snprintf(command, sizeof(command),
                 "cc -std=c11 -no-pie -Iinclude -Isamples/smalltalk/include "
                 "%s %s -o %s >/dev/null 2>&1",
                 asm_path, harness_path, executable_path);
        CHECK(system(command) == 0,
              "x64 Smalltalk dispatch assembly should link with the C ABI frame");
        snprintf(command, sizeof(command), "%s", executable_path);
        CHECK(system(command) == 0,
              "both Smalltalk VTable slots should execute with receiver/args intact");
    }
    unlink(asm_path);
    unlink(harness_path);
    unlink(executable_path);
#else
    (void)assembly;
#endif
}

static bool cross_assemble(const char *assembly, const char *triple)
{
    if (!assembly || !triple || access("/usr/bin/clang", X_OK) != 0)
        return true;
    char source[] = "/tmp/anvil-smalltalk-cross-XXXXXX.s";
    int fd = mkstemps(source, 2);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "wb");
    bool ok = file && fputs(assembly, file) >= 0;
    if (file) ok = fclose(file) == 0 && ok;
    else close(fd);
    char object[192], command[640];
    snprintf(object, sizeof(object), "%s.o", source);
    snprintf(command, sizeof(command),
             "/usr/bin/clang --target=%s -c %s -o %s",
             triple, source, object);
    if (ok) ok = system(command) == 0;
    unlink(source);
    unlink(object);
    return ok;
}

static char *build_kernel(anvil_arch_t arch,
                          st_dispatch_kernel_t *kernel_out,
                          anvil_ctx_t **ctx_out)
{
    *ctx_out = NULL;
    memset(kernel_out, 0, sizeof(*kernel_out));
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    CHECK(ctx != NULL, "64-bit target context should construct");
    if (!ctx) return NULL;
    if (!st_dispatch_kernel_build(ctx, kernel_out)) {
        fprintf(stderr, "kernel build error: %s\n", anvil_ctx_get_error(ctx));
        CHECK(false, "Smalltalk dispatch kernel should build completely");
        anvil_ctx_destroy(ctx);
        return NULL;
    }

    CHECK(anvil_type_size(kernel_out->frame_type) == sizeof(StFrame) &&
          anvil_type_align(kernel_out->frame_type) == _Alignof(StFrame) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_ARGC_FIELD) ==
              offsetof(StFrame, argc) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_CALLER_FIELD) ==
              offsetof(StFrame, caller) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_ROOTS_FIELD) ==
              offsetof(StFrame, roots) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_ROOT_COUNT_FIELD) ==
              offsetof(StFrame, root_count) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_METHOD_FIELD) ==
              offsetof(StFrame, method) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_HOME_FIELD) ==
              offsetof(StFrame, home) &&
          anvil_type_struct_field_offset(kernel_out->frame_type,
                                         ST_FRAME_SAFEPOINT_FIELD) ==
              offsetof(StFrame, safepoint_id),
          "C and Anvil StFrame layouts must agree exactly");
    CHECK(anvil_type_func_cc_value(kernel_out->method_type) == ANVIL_CC_SYSV,
          "64-bit ARM/PPC/x64 method types must retain canonical SysV CC");
    CHECK(kernel_out->vtable &&
          kernel_out->vtable->data.global.init &&
          kernel_out->vtable->data.global.init->kind ==
              ANVIL_VAL_CONST_ARRAY &&
          kernel_out->vtable->data.global.init->owner_module ==
              kernel_out->module,
          "method VTable must be a module-owned relocatable aggregate");

    anvil_instr_t *call = find_indirect_call(kernel_out->dispatcher);
    anvil_instr_t *miss_call = find_direct_call(
        kernel_out->dispatcher, "st_dispatch_miss");
    CHECK(call && call->call_cc == ANVIL_CC_SYSV,
          "dispatcher indirect CALL must preserve the method type CC");
    CHECK(miss_call && miss_call->call_cc == ANVIL_CC_SYSV,
          "dispatcher miss hook CALL must preserve its typed CC");
    char verification[256] = { 0 };
    CHECK(anvil_module_verify(kernel_out->module, verification,
                              sizeof(verification)),
          verification[0] ? verification :
              "Smalltalk dispatch module should verify");

    char *assembly = NULL;
    size_t length = 0;
    anvil_error_t status = anvil_module_codegen(
        kernel_out->module, &assembly, &length);
    if (status != ANVIL_OK) {
        fprintf(stderr, "kernel codegen error: %s\n", anvil_ctx_get_error(ctx));
    }
    CHECK(status == ANVIL_OK && assembly && length > 0,
          "Smalltalk dispatch kernel should emit assembly");
    *ctx_out = ctx;
    return assembly;
}

static void test_x86_64_runtime(void)
{
    st_dispatch_kernel_t kernel;
    anvil_ctx_t *ctx;
    char *assembly = build_kernel(ANVIL_ARCH_X86_64, &kernel, &ctx);
    if (!ctx) return;
    if (assembly) {
        CHECK(strstr(assembly, "st_method_vtable") &&
              strstr(assembly, "st_method_receiver_plus_argc") &&
              strstr(assembly, "st_method_receiver_plus_first_arg") &&
              strstr(assembly, "call *") &&
              strstr(assembly, "call st_dispatch_miss"),
              "x64 output must contain VTable dispatch and the explicit miss hook");
        run_x64_dispatch(assembly);
    }
    st_dispatch_kernel_t second_kernel;
    CHECK(st_dispatch_kernel_build(ctx, &second_kernel) &&
          second_kernel.frame_type == kernel.frame_type,
          "multiple modules in one context must reuse the nominal StFrame ABI");
    if (second_kernel.module) anvil_module_destroy(second_kernel.module);
    free(assembly);
    anvil_module_destroy(kernel.module);
    anvil_ctx_destroy(ctx);
}

static void test_cross_targets(void)
{
    struct cross_case {
        anvil_arch_t arch;
        const char *triple;
        const char *name;
    } cases[] = {
        { ANVIL_ARCH_ARM64, "aarch64-linux-gnu", "ARM64" }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        st_dispatch_kernel_t kernel;
        anvil_ctx_t *ctx;
        char *assembly = build_kernel(cases[i].arch, &kernel, &ctx);
        if (!ctx) continue;
        char message[128];
        snprintf(message, sizeof(message),
                 "%s Smalltalk dispatch output should cross-assemble",
                 cases[i].name);
        CHECK(assembly && cross_assemble(assembly, cases[i].triple), message);
        free(assembly);
        anvil_module_destroy(kernel.module);
        anvil_ctx_destroy(ctx);
    }
}

static void test_build_fault_injection(void)
{
    bool reached_success = false;
    for (size_t fail_after = 0; fail_after < 512; fail_after++) {
        anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
        CHECK(ctx != NULL, "fault-injection target context should construct");
        if (!ctx) continue;
        st_dispatch_kernel_t kernel;
        anvil_ctx_clear_error(ctx);
        anvil_test_fail_alloc_after(ctx, fail_after);
        bool built = st_dispatch_kernel_build(ctx, &kernel);
        anvil_test_disable_alloc_fail(ctx);
        if (built) {
            reached_success = true;
            CHECK(kernel.module != NULL,
                  "successful fault sweep must publish a complete kernel");
            anvil_module_destroy(kernel.module);
            anvil_ctx_destroy(ctx);
            break;
        }
        if (kernel.module != NULL ||
            anvil_ctx_get_last_error(ctx) != ANVIL_ERR_NOMEM) {
            fprintf(stderr,
                    "fault sweep %zu: module=%p error=%d (%s)\n",
                    fail_after, (void *)kernel.module,
                    (int)anvil_ctx_get_last_error(ctx),
                    anvil_ctx_get_error(ctx));
            CHECK(false,
                  "kernel OOM must fail without publishing a partial module");
        }
        anvil_ctx_destroy(ctx);
    }
    CHECK(reached_success,
          "fault-injection sweep must reach the complete kernel path");
}

int main(void)
{
    test_x86_64_runtime();
    test_cross_targets();
    test_build_fault_injection();
    if (failures) {
        fprintf(stderr, "%d Smalltalk dispatch regression(s) failed\n",
                failures);
        return 1;
    }
    puts("smalltalk dispatch: PASS (x86_64 runtime, ARM64 assembly)");
    return 0;
}
