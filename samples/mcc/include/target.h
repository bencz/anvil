#ifndef MCC_TARGET_H
#define MCC_TARGET_H

#include "mcc.h"

/* C data-model policy belongs to the generated target, never the host CRT. */
typedef struct
{
    const char *name;
    const char *value;
} mcc_target_macro_t;

typedef struct
{
    anvil_abi_t abi;
    bool long_matches_pointer;
    bool native_aggregate_plans;
    const char *size_type;
    const char *ptrdiff_type;
    const char *wchar_type;
    const char *include_subdirectory;
    const mcc_target_macro_t *macros;
    size_t macro_count;
} mcc_target_model_t;

const mcc_target_model_t *mcc_target_model(mcc_arch_t arch);

#endif
