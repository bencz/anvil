#ifndef ANVIL_X86_64_SYSV_H
#define ANVIL_X86_64_SYSV_H

#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const AnvilABI x86_64_sysv_abi;

void x86_64_sysv_classify_argument(const AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, int arg_index, AnvilArgInfo* out);
void x86_64_sysv_classify_return(const AnvilABI* abi, const AnvilTargetInfo* target,
                                  AnvilType* type, AnvilArgInfo* out);
void x86_64_sysv_compute_frame_layout(const AnvilABI* abi, const AnvilTargetInfo* target,
                                       AnvilMFunc* func, AnvilFrameLayout* out);

#ifdef __cplusplus
}
#endif

#endif
