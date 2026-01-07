#ifndef ANVIL_ARM64_AAPCS64_H
#define ANVIL_ARM64_AAPCS64_H

#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const AnvilABI arm64_aapcs64_abi;

void arm64_aapcs64_classify_argument(const AnvilABI* abi, const AnvilTargetInfo* target,
                                      AnvilType* type, int arg_index, AnvilArgInfo* out);
void arm64_aapcs64_classify_return(const AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, AnvilArgInfo* out);
void arm64_aapcs64_compute_frame_layout(const AnvilABI* abi, const AnvilTargetInfo* target,
                                         AnvilMFunc* func, AnvilFrameLayout* out);

#ifdef __cplusplus
}
#endif

#endif
