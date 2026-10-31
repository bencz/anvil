#include "systemz_internal.h"

/* R0/R1 (the even/odd pair of multiply and divide), R14 and F0 are the
 * emitter's own temporaries, so spilled values are reloaded elsewhere. */
const int systemz_alloc_gprs[] = {2, 3, 4, 5, 6, 7, 8};
const int systemz_scratch_gprs[] = {9, 10};
/* FPRs by CPU facility: F0, F2, F4 and F6 on every model; F1-F15 (the
 * additional floating-point registers) arrived with the BFP facility on the
 * S/390 G5.  F0 is the emitter's temporary; an instruction may need a
 * scratch register for each spilled FP operand, hence two. */
const int systemz_s370_fprs[] = {2};
const int systemz_s370_scratch_fprs[] = {4, 6};
const int systemz_s390_fprs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
const int systemz_s390_scratch_fprs[] = {14, 15};

static const anvil_mainframe_target_desc_t *const targets[] = {&s370_target_desc, &s370_xa_target_desc, &s390_target_desc, &zarch_target_desc};

const anvil_mainframe_target_desc_t *anvil_mainframe_get_target_desc(anvil_mainframe_variant_t variant)
{
    if ((size_t)variant >= sizeof(targets) / sizeof(targets[0]))
        return NULL;

    return targets[variant];
}
