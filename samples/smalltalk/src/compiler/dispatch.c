#include "st_dispatch.h"

#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(StFrame, thread) == 0,
               "StFrame.thread ABI offset changed");
_Static_assert(offsetof(StFrame, caller) == 8,
               "StFrame.caller ABI offset changed");
_Static_assert(offsetof(StFrame, method) == 16,
               "StFrame.method ABI offset changed");
_Static_assert(offsetof(StFrame, home) == 24,
               "StFrame.home ABI offset changed");
_Static_assert(offsetof(StFrame, receiver) == 32,
               "StFrame.receiver ABI offset changed");
_Static_assert(offsetof(StFrame, argv) == 40,
               "StFrame.argv ABI offset changed");
_Static_assert(offsetof(StFrame, roots) == 48,
               "StFrame.roots ABI offset changed");
_Static_assert(offsetof(StFrame, argc) == 56,
               "StFrame.argc ABI offset changed");
_Static_assert(offsetof(StFrame, root_count) == 60,
               "StFrame.root_count ABI offset changed");
_Static_assert(offsetof(StFrame, safepoint_id) == 64,
               "StFrame.safepoint_id ABI offset changed");
_Static_assert(offsetof(StFrame, flags) == 68,
               "StFrame.flags ABI offset changed");
_Static_assert(sizeof(StFrame) == 72, "StFrame ABI size changed");

static bool frame_layout_matches_c(anvil_type_t *frame_type)
{
    return frame_type && anvil_type_size(frame_type) == sizeof(StFrame) &&
           anvil_type_align(frame_type) == _Alignof(StFrame) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_THREAD_FIELD) ==
               offsetof(StFrame, thread) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_CALLER_FIELD) ==
               offsetof(StFrame, caller) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_METHOD_FIELD) ==
               offsetof(StFrame, method) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_HOME_FIELD) ==
               offsetof(StFrame, home) &&
           anvil_type_struct_field_offset(frame_type,
                                          ST_FRAME_RECEIVER_FIELD) ==
               offsetof(StFrame, receiver) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_ARGV_FIELD) ==
               offsetof(StFrame, argv) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_ARGC_FIELD) ==
               offsetof(StFrame, argc) &&
           anvil_type_struct_field_offset(frame_type,
                                          ST_FRAME_ROOTS_FIELD) ==
               offsetof(StFrame, roots) &&
           anvil_type_struct_field_offset(frame_type,
                                          ST_FRAME_ROOT_COUNT_FIELD) ==
               offsetof(StFrame, root_count) &&
           anvil_type_struct_field_offset(frame_type,
                                          ST_FRAME_SAFEPOINT_FIELD) ==
               offsetof(StFrame, safepoint_id) &&
           anvil_type_struct_field_offset(frame_type, ST_FRAME_FLAGS_FIELD) ==
               offsetof(StFrame, flags);
}

static anvil_value_t *load_frame_field(anvil_ctx_t *ctx,
                                       anvil_type_t *frame_type,
                                       anvil_value_t *frame,
                                       unsigned field,
                                       anvil_type_t *field_type,
                                       const char *pointer_name,
                                       const char *value_name)
{
    anvil_value_t *address = anvil_build_struct_gep(
        ctx, frame_type, frame, field, pointer_name);
    return address ? anvil_build_load(ctx, field_type, address, value_name)
                   : NULL;
}

static anvil_func_t *build_receiver_plus_argc(anvil_ctx_t *ctx,
                                              anvil_module_t *module,
                                              anvil_type_t *frame_type,
                                              anvil_type_t *method_type)
{
    anvil_func_t *method = anvil_func_create(
        module, "st_method_receiver_plus_argc", method_type,
        ANVIL_LINK_INTERNAL);
    if (!method || !anvil_set_insert_point(ctx, anvil_func_get_entry(method)))
        return NULL;

    anvil_value_t *frame = anvil_func_get_param(method, 0);
    anvil_value_t *receiver = load_frame_field(
        ctx, frame_type, frame, ST_FRAME_RECEIVER_FIELD, anvil_type_u64(ctx),
        "receiver.addr", "receiver");
    anvil_value_t *argc = load_frame_field(
        ctx, frame_type, frame, ST_FRAME_ARGC_FIELD, anvil_type_u32(ctx),
        "argc.addr", "argc");
    anvil_value_t *argc64 = argc
        ? anvil_build_zext(ctx, argc, anvil_type_u64(ctx), "argc.u64") : NULL;
    anvil_value_t *result = receiver && argc64
        ? anvil_build_add(ctx, receiver, argc64, "receiver.plus.argc") : NULL;
    return result && anvil_build_ret(ctx, result) ? method : NULL;
}

static anvil_func_t *build_receiver_plus_first_arg(anvil_ctx_t *ctx,
                                                   anvil_module_t *module,
                                                   anvil_type_t *frame_type,
                                                   anvil_type_t *method_type)
{
    anvil_func_t *method = anvil_func_create(
        module, "st_method_receiver_plus_first_arg", method_type,
        ANVIL_LINK_INTERNAL);
    if (!method || !anvil_set_insert_point(ctx, anvil_func_get_entry(method)))
        return NULL;

    anvil_type_t *u64 = anvil_type_u64(ctx);
    anvil_type_t *argv_type = anvil_type_ptr(ctx, u64);
    if (!u64 || !argv_type) return NULL;
    anvil_value_t *frame = anvil_func_get_param(method, 0);
    anvil_value_t *receiver = load_frame_field(
        ctx, frame_type, frame, ST_FRAME_RECEIVER_FIELD, u64,
        "receiver.addr", "receiver");
    anvil_value_t *argc = load_frame_field(
        ctx, frame_type, frame, ST_FRAME_ARGC_FIELD, anvil_type_u32(ctx),
        "argc.addr", "argc");
    anvil_value_t *zero_argc = argc ? anvil_const_u32(ctx, 0) : NULL;
    anvil_value_t *has_arg = zero_argc
        ? anvil_build_cmp_ne(ctx, argc, zero_argc, "has.arg0") : NULL;
    anvil_block_t *with_arg = anvil_block_create(method, "with.arg0");
    anvil_block_t *without_arg = anvil_block_create(method, "without.arg0");
    if (!receiver || !has_arg || !with_arg || !without_arg ||
        !anvil_build_br_cond(ctx, has_arg, with_arg, without_arg)) return NULL;

    if (!anvil_set_insert_point(ctx, without_arg) ||
        !anvil_build_ret(ctx, receiver)) return NULL;

    if (!anvil_set_insert_point(ctx, with_arg)) return NULL;
    anvil_value_t *argv = load_frame_field(
        ctx, frame_type, frame, ST_FRAME_ARGV_FIELD, argv_type,
        "argv.addr", "argv");
    anvil_value_t *zero = anvil_const_u64(ctx, 0);
    anvil_value_t *indices[] = { zero };
    anvil_value_t *arg_address = argv && zero
        ? anvil_build_gep(ctx, u64, argv, indices, 1, "arg0.addr") : NULL;
    anvil_value_t *arg = arg_address
        ? anvil_build_load(ctx, u64, arg_address, "arg0") : NULL;
    anvil_value_t *result = arg
        ? anvil_build_add(ctx, receiver, arg, "receiver.plus.arg0") : NULL;
    return result && anvil_build_ret(ctx, result) ? method : NULL;
}

static anvil_func_t *build_dispatcher(anvil_ctx_t *ctx,
                                      anvil_module_t *module,
                                      anvil_type_t *frame_type,
                                      anvil_type_t *method_ptr_type,
                                      anvil_type_t *vtable_type,
                                      anvil_value_t *vtable)
{
    anvil_type_t *frame_ptr = anvil_type_ptr(ctx, frame_type);
    if (!frame_ptr) return NULL;
    anvil_type_t *params[] = { frame_ptr, anvil_type_u32(ctx) };
    anvil_type_t *dispatcher_type = anvil_type_func(
        ctx, anvil_type_u64(ctx), params, 2, false);
    if (!dispatcher_type) return NULL;
    anvil_func_t *miss = anvil_func_declare(
        module, "st_dispatch_miss", dispatcher_type);
    anvil_func_t *dispatcher = dispatcher_type
        ? anvil_func_create(module, "st_dispatch", dispatcher_type,
                            ANVIL_LINK_EXTERNAL) : NULL;
    if (!miss || !dispatcher ||
        !anvil_set_insert_point(ctx, anvil_func_get_entry(dispatcher))) {
        return NULL;
    }

    anvil_value_t *frame = anvil_func_get_param(dispatcher, 0);
    anvil_value_t *slot = anvil_func_get_param(dispatcher, 1);
    anvil_block_t *hit = anvil_block_create(dispatcher, "dispatch.hit");
    anvil_block_t *miss_block = anvil_block_create(dispatcher, "dispatch.miss");
    anvil_value_t *method_count = anvil_const_u32(
        ctx, ST_DISPATCH_METHOD_COUNT);
    anvil_value_t *in_range = method_count
        ? anvil_build_cmp_ult(ctx, slot, method_count, "slot.in.range") : NULL;
    if (!hit || !miss_block || !in_range ||
        !anvil_build_br_cond(ctx, in_range, hit, miss_block)) return NULL;

    if (!anvil_set_insert_point(ctx, hit)) return NULL;
    anvil_value_t *vtable_address = anvil_const_symbol_addr(vtable);
    anvil_value_t *zero = vtable_address ? anvil_const_u32(ctx, 0) : NULL;
    anvil_value_t *indices[] = { zero, slot };
    anvil_value_t *slot_address = vtable_address && zero
        ? anvil_build_gep(ctx, vtable_type, vtable_address, indices, 2,
                          "method.slot.addr") : NULL;
    anvil_value_t *callee = slot_address
        ? anvil_build_load(ctx, method_ptr_type, slot_address, "method") : NULL;
    anvil_value_t *args[] = { frame };
    anvil_value_t *result = NULL;
    if (!callee ||
        !anvil_build_call_checked(ctx, callee, args, 1, "method.result",
                                  &result) ||
        !result || !anvil_build_ret(ctx, result)) return NULL;

    if (!anvil_set_insert_point(ctx, miss_block)) return NULL;
    anvil_value_t *miss_args[] = { frame, slot };
    result = NULL;
    return anvil_build_call_checked(ctx, anvil_func_get_value(miss),
                                    miss_args, 2, "miss.result", &result) &&
           result && anvil_build_ret(ctx, result) ? dispatcher : NULL;
}

bool st_dispatch_kernel_build(anvil_ctx_t *ctx,
                              st_dispatch_kernel_t *kernel)
{
    if (!kernel) return false;
    memset(kernel, 0, sizeof(*kernel));
    if (!ctx) return false;
    const anvil_data_layout_t *layout = anvil_ctx_get_data_layout(ctx);
    if (!layout || layout->pointer.size != 8) return false;

    anvil_module_t *module = anvil_module_create(ctx, "smalltalk.dispatch");
    if (!module) return false;

    anvil_type_t *u64 = anvil_type_u64(ctx);
    anvil_type_t *u32 = anvil_type_u32(ctx);
    anvil_type_t *thread_ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *argv_ptr = thread_ptr ? anvil_type_ptr(ctx, u64) : NULL;
    anvil_type_t *roots_ptr = argv_ptr ? anvil_type_ptr(ctx, u64) : NULL;
    if (!thread_ptr || !argv_ptr || !roots_ptr) goto fail;
    anvil_type_t *frame_fields[] = {
        thread_ptr,
        NULL,
        thread_ptr,
        thread_ptr,
        u64,
        argv_ptr,
        roots_ptr,
        u32,
        u32,
        u32,
        u32
    };
    anvil_type_t *frame_type = anvil_type_named_struct(ctx, "StFrame");
    anvil_type_t *frame_ptr = frame_type
        ? anvil_type_ptr(ctx, frame_type) : NULL;
    if (!frame_type || !frame_ptr) goto fail;
    frame_fields[ST_FRAME_CALLER_FIELD] = frame_ptr;
    if (anvil_type_struct_is_opaque(frame_type)) {
        if (!anvil_type_struct_set_body(frame_type, frame_fields, 11, false))
            goto fail;
    } else if (anvil_type_struct_field_count(frame_type) != 11) {
        goto fail;
    }
    anvil_type_t *method_params[] = { frame_ptr };
    anvil_type_t *method_type = frame_ptr
        ? anvil_type_func(ctx, u64, method_params, 1, false) : NULL;
    anvil_type_t *method_ptr_type = method_type
        ? anvil_type_ptr(ctx, method_type) : NULL;
    if (!frame_layout_matches_c(frame_type) || !method_ptr_type) goto fail;

    anvil_func_t *method0 = build_receiver_plus_argc(
        ctx, module, frame_type, method_type);
    anvil_func_t *method1 = build_receiver_plus_first_arg(
        ctx, module, frame_type, method_type);
    if (!method0 || !method1) goto fail;

    anvil_value_t *method_addresses[] = { NULL, NULL };
    method_addresses[0] = anvil_const_symbol_addr(
        anvil_func_get_value(method0));
    if (!method_addresses[0]) goto fail;
    method_addresses[1] = anvil_const_symbol_addr(
        anvil_func_get_value(method1));
    if (!method_addresses[0] || !method_addresses[1]) goto fail;
    anvil_value_t *vtable_initializer = anvil_const_array(
        ctx, method_ptr_type, method_addresses, ST_DISPATCH_METHOD_COUNT);
    anvil_type_t *vtable_type = vtable_initializer
        ? anvil_value_get_type(vtable_initializer) : NULL;
    anvil_value_t *vtable = vtable_type
        ? anvil_module_add_global(module, "st_method_vtable", vtable_type,
                                  ANVIL_LINK_INTERNAL) : NULL;
    if (!vtable || !anvil_global_set_initializer(vtable, vtable_initializer))
        goto fail;

    anvil_func_t *dispatcher = build_dispatcher(
        ctx, module, frame_type, method_ptr_type, vtable_type, vtable);
    if (!dispatcher) goto fail;

    kernel->module = module;
    kernel->frame_type = frame_type;
    kernel->method_type = method_type;
    kernel->method_ptr_type = method_ptr_type;
    kernel->vtable_type = vtable_type;
    kernel->vtable = vtable;
    kernel->methods[0] = method0;
    kernel->methods[1] = method1;
    kernel->dispatcher = dispatcher;
    return true;

fail:
    anvil_module_destroy(module);
    return false;
}
