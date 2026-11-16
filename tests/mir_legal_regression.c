/* One MachineIR legality checker for every target (backend/common/mir_legal.c),
 * fed each target's facts: on x86-64, i386, ARM64, PowerPC 32/64 and the
 * SystemZ family it accepts a pointer-wide load, and rejects an integer-only
 * operation (AND) on floating-point registers, a load through a base narrower
 * than a pointer, and an opcode the target never takes (VA_START on SystemZ,
 * whose own checker used to accept any opcode it did not name). */
#include "anvil/anvil_machine.h"
#include "anvil/anvil_x86_64_mir.h"
#include "anvil/anvil_x86_mir.h"
#include "anvil/anvil_arm64_mir.h"
#include "anvil/anvil_ppc_mir.h"
#include "anvil/anvil_mainframe_mir.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, "[FAIL] %s\n", msg); \
            failures++;                          \
        }                                        \
    } while (0)

static bool verify_x86_64(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_x86_64_verify_mir_legal(mir, error, length);
}

static bool verify_x86(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_x86_verify_mir_legal(mir, error, length);
}

static bool verify_arm64(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_arm64_verify_mir_legal(mir, error, length);
}

static bool verify_ppc32(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_ppc_verify_mir_legal(mir, ANVIL_PPC_VARIANT_PPC32, error, length);
}

static bool verify_ppc64(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_ppc_verify_mir_legal(mir, ANVIL_PPC_VARIANT_PPC64, error, length);
}

static bool verify_s390(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_mainframe_verify_mir_legal(mir, ANVIL_MAINFRAME_VARIANT_S390, error, length);
}

static bool verify_zarch(const anvil_mir_func_t *mir, char *error, size_t length)
{
    return anvil_mainframe_verify_mir_legal(mir, ANVIL_MAINFRAME_VARIANT_ZARCH, error, length);
}

typedef struct {
    const char *name;
    bool (*verify)(const anvil_mir_func_t *mir, char *error, size_t length);
    uint16_t pointer_bits;
    bool mainframe;
} target_t;

static const target_t targets[] = {
    {"x86-64", verify_x86_64, 64, false}, {"i386", verify_x86, 32, false},  {"ARM64", verify_arm64, 64, false}, {"ppc32", verify_ppc32, 32, false},
    {"ppc64", verify_ppc64, 64, false},   {"S/390", verify_s390, 32, true}, {"z/Arch", verify_zarch, 64, true},
};

typedef enum { CASE_LEGAL_LOAD, CASE_FP_AND, CASE_NARROW_BASE, CASE_FOREIGN_OPCODE } case_t;

/* value = load base; ret value — with the base, the operands or the opcode
 * the case asks for. */
static anvil_mir_func_t *build(const target_t *target, case_t kind)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("legal");
    if (!mir)
        return NULL;
    anvil_mir_vreg_t base = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, kind == CASE_NARROW_BASE ? 16 : target->pointer_bits);
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t base_use[] = {base};
    bool ok = anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, base, 4096) && anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, value, base_use, 1);
    if (kind == CASE_FP_AND) {
        anvil_mir_vreg_t fp = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
        anvil_mir_vreg_t masked = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
        anvil_mir_vreg_t uses[] = {fp, fp};
        ok = ok && anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, fp, base_use, 1) && anvil_mir_add_instr(mir, ANVIL_MIR_OP_AND, masked, uses, 2);
        value = masked;
    }
    if (kind == CASE_FOREIGN_OPCODE) {
        anvil_mir_vreg_t cursor = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, target->pointer_bits);
        ok = ok && anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_VA_START, cursor, 0);
    }
    anvil_mir_vreg_t ret_use[] = {value};
    ok = ok && anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_use, 1);
    if (!ok) {
        anvil_mir_func_destroy(mir);
        return NULL;
    }
    return mir;
}

static void expect(const target_t *target, case_t kind, bool legal, const char *reason, const char *what)
{
    anvil_mir_func_t *mir = build(target, kind);
    char error[256] = {0};
    char message[320];
    bool verified = mir && target->verify(mir, error, sizeof(error));
    snprintf(message, sizeof(message), "%s: %s (%s)", target->name, what, error);
    CHECK(mir && verified == legal && (legal || !reason || strstr(error, reason)), message);
    anvil_mir_func_destroy(mir);
}

int main(void)
{
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        const target_t *target = &targets[t];
        expect(target, CASE_LEGAL_LOAD, true, NULL, "a load through a pointer-wide base is legal");
        expect(target, CASE_FP_AND, false, "illegal binary opcode/class pair", "an AND of FP registers is rejected");
        expect(target, CASE_NARROW_BASE, false, "pointer", "a load through a 16-bit base is rejected");
        if (target->mainframe)
            expect(target, CASE_FOREIGN_OPCODE, false, "unsupported opcode", "VA_START is rejected");
    }
    if (failures) {
        fprintf(stderr, "%d MIR legality regression check(s) failed\n", failures);
        return 1;
    }
    printf("MIR legality regression tests passed\n");
    return 0;
}
