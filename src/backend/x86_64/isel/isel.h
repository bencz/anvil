#ifndef ANVIL_X86_64_ISEL_H
#define ANVIL_X86_64_ISEL_H

#include "../../../mir/isel.h"
#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const AnvilISelRuleSet x86_64_isel_ruleset;

void x86_64_isel_run(AnvilBackend* backend, AnvilMFunc* func);

#ifdef __cplusplus
}
#endif

#endif
