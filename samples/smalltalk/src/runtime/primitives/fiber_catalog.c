#include "st_fiber_primitives.h"

static const st_primitive_spec_t specifications[] = {
    {
        .name = "FiberSpawnPrimitive",
        .name_length = sizeof("FiberSpawnPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_spawn",
        .runtime_symbol_length = sizeof("st_aot_fiber_spawn") - 1u
    },
    {
        .name = "FiberYieldPrimitive",
        .name_length = sizeof("FiberYieldPrimitive") - 1u,
        .method_arity = 0u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_yield",
        .runtime_symbol_length = sizeof("st_aot_fiber_yield") - 1u
    },
    {
        .name = "FiberSleepPrimitive",
        .name_length = sizeof("FiberSleepPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_sleep",
        .runtime_symbol_length = sizeof("st_aot_fiber_sleep") - 1u
    },
    {
        .name = "FiberJoinPrimitive",
        .name_length = sizeof("FiberJoinPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_join",
        .runtime_symbol_length = sizeof("st_aot_fiber_join") - 1u
    },
    {
        .name = "FiberRunPrimitive",
        .name_length = sizeof("FiberRunPrimitive") - 1u,
        .method_arity = 0u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_run",
        .runtime_symbol_length = sizeof("st_aot_fiber_run") - 1u
    },
    {
        .name = "FiberDetachPrimitive",
        .name_length = sizeof("FiberDetachPrimitive") - 1u,
        .method_arity = 1u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_detach",
        .runtime_symbol_length = sizeof("st_aot_fiber_detach") - 1u
    },
    {
        .name = "MemoryCollectPrimitive",
        .name_length = sizeof("MemoryCollectPrimitive") - 1u,
        .method_arity = 0u,
        .receiver_policy = ST_PRIMITIVE_CLASS_ONLY,
        .failure_policy = ST_PRIMITIVE_FALL_THROUGH,
        .implementation_kind = ST_PRIMITIVE_RUNTIME_SYMBOL,
        .runtime_symbol = "st_aot_fiber_collect",
        .runtime_symbol_length = sizeof("st_aot_fiber_collect") - 1u
    },
};

const st_primitive_spec_t *st_fiber_primitive_specs(size_t *count_out)
{
    if (count_out != NULL)
        *count_out = sizeof(specifications) / sizeof(specifications[0]);

    return specifications;
}

