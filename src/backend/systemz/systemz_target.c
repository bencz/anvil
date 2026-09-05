#include "systemz_internal.h"

const int systemz_alloc_gprs[] = {2, 3, 4, 5, 6, 7, 8, 9, 10};
const int systemz_scratch_gprs[] = {0, 1};
const int systemz_s370_fprs[] = {2, 4, 6};
const int systemz_s370_scratch_fprs[] = {0};
const int systemz_s390_fprs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
const int systemz_s390_scratch_fprs[] = {0};

static const anvil_mainframe_target_desc_t *const targets[] = {&s370_target_desc, &s370_xa_target_desc, &s390_target_desc, &zarch_target_desc};

const anvil_mainframe_target_desc_t *anvil_mainframe_get_target_desc(anvil_mainframe_variant_t variant)
{
    if ((size_t)variant >= sizeof(targets) / sizeof(targets[0]))
        return NULL;

    return targets[variant];
}
