#ifndef ANVIL_SMALLTALK_CONTROL_ROOTS_H
#define ANVIL_SMALLTALK_CONTROL_ROOTS_H

#include "st_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Authenticates the AOT thread stored in top_frame, its immutable control
 * sidecar link, and then delegates exact root enumeration to st_control.
 * Neither layer allocates. */
st_control_status_t st_aot_control_visit_roots(
    const StFrame *top_frame, st_control_root_visitor_fn visitor,
    void *user, size_t *visited_out);

#ifdef __cplusplus
}
#endif

#endif
