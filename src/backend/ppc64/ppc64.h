#ifndef ANVIL_PPC64_H
#define ANVIL_PPC64_H

#include "../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

AnvilBackend* anvil_create_ppc64_backend(void);

const AnvilABI* ppc64_get_abi(int os, const char* abi_name);

#ifdef __cplusplus
}
#endif

#endif
