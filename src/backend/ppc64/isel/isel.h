#ifndef ANVIL_PPC64_ISEL_H
#define ANVIL_PPC64_ISEL_H

#include "../../../mir/isel.h"
#include "../../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const AnvilISelRuleSet ppc64_isel_ruleset;

void ppc64_isel_run(AnvilBackend* backend, AnvilMFunc* func);

#ifdef __cplusplus
}
#endif

#endif
