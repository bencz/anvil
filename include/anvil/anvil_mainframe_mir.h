/*
 * ANVIL - Shared IBM mainframe MachineIR backend interface.
 *
 * S/370, S/370-XA, S/390, and z/Architecture use one lowering,
 * allocation, and HLASM emission pipeline. Target descriptors hold the
 * architectural and ABI facts that differ between variants.
 */

#ifndef ANVIL_MAINFRAME_MIR_H
#define ANVIL_MAINFRAME_MIR_H

#include "anvil.h"
#include "anvil_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANVIL_MAINFRAME_VARIANT_S370 = 0,
    ANVIL_MAINFRAME_VARIANT_S370_XA,
    ANVIL_MAINFRAME_VARIANT_S390,
    ANVIL_MAINFRAME_VARIANT_ZARCH
} anvil_mainframe_variant_t;

typedef struct {
    anvil_mainframe_variant_t variant;
    anvil_arch_t arch;
    const char *name;
    unsigned ptr_size;
    unsigned addr_bits;
    unsigned word_size;
    unsigned num_fpr;
    unsigned save_area_size;
    unsigned fp_temp_offset;
    unsigned local_area_offset;
    const char *amode;
    const char *rmode;
    anvil_fp_format_t fp_format;
    bool big_endian;
    bool has_64bit_gprs;
    bool supports_ieee_fp;
    bool supports_dfp;
    bool has_relative_branches;
    int param_list_reg;
    int return_addr_reg;
    int return_gpr;
    int return_fpr;
    int base_reg;
    int save_area_reg;
    const int *alloc_gpr_regs;
    size_t num_alloc_gpr_regs;
    const int *alloc_fpr_regs;
    size_t num_alloc_fpr_regs;
    const int *scratch_gpr_regs;
    size_t num_scratch_gpr_regs;
    const int *scratch_fpr_regs;
    size_t num_scratch_fpr_regs;
} anvil_mainframe_target_desc_t;

const anvil_mainframe_target_desc_t *
anvil_mainframe_get_target_desc(anvil_mainframe_variant_t variant);

anvil_mir_func_t *anvil_mainframe_lower_func_to_mir(
    anvil_func_t *func,
    anvil_mainframe_variant_t variant);
bool anvil_mainframe_verify_mir_legal(
    const anvil_mir_func_t *mir,
    anvil_mainframe_variant_t variant,
    char *error,
    size_t error_len);
bool anvil_mainframe_regalloc_mir(anvil_mir_func_t *mir,
                                  anvil_mainframe_variant_t variant);
bool anvil_mainframe_emit_mir(const anvil_mir_func_t *mir,
                              anvil_mainframe_variant_t variant,
                              char **output,
                              size_t *len);

anvil_error_t anvil_mainframe_codegen_func(anvil_backend_t *be,
                                           anvil_func_t *func,
                                           anvil_mainframe_variant_t variant,
                                           char **output,
                                           size_t *len);
anvil_error_t anvil_mainframe_codegen_module(anvil_backend_t *be,
                                             anvil_module_t *mod,
                                             anvil_mainframe_variant_t variant,
                                             char **output,
                                             size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_MAINFRAME_MIR_H */
