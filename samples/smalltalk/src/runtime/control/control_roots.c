#include "st_control_roots.h"

#include "st_send_bridge.h"

static bool lookup_is_ready(const st_lookup_context_t *lookup)
{
    return lookup != NULL && lookup->initialized && lookup->descriptors != NULL
        && lookup->class_epochs != NULL;
}

st_control_status_t st_aot_control_visit_roots(
    const StFrame *top_frame, st_control_root_visitor_fn visitor,
    void *user, size_t *visited_out)
{
    const st_aot_thread_t *thread;
    const st_control_thread_t *control;
    if (visited_out) *visited_out = 0u;
    if (top_frame == NULL || visitor == NULL || visited_out == NULL
            || top_frame->thread == NULL)
        return ST_CONTROL_ERR_INVALID_ARGUMENT;
    thread = top_frame->thread;
    if (!thread->initialized ||
        thread->abi_version != ST_AOT_THREAD_ABI_VERSION ||
        !lookup_is_ready(thread->lookup) || thread->control == NULL)
        return ST_CONTROL_ERR_INVALID_THREAD;
    control = thread->control;
    if (control->_st_abi_version != ST_CONTROL_ABI_VERSION ||
        control->_st_thread_id == 0u ||
        control->_st_frame_thread_identity != thread ||
        !control->_st_allocator.allocate || !control->_st_allocator.deallocate)
        return ST_CONTROL_ERR_INVALID_THREAD;
    return st_control_visit_roots(control, top_frame, visitor, user,
                                  visited_out);
}
