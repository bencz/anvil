#include "st_lookup.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(ST_PIC_WAYS == 4u,
               "the send-site ABI requires a four-way PIC");

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static void clear_result(st_lookup_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));
}

static bool context_is_valid(const st_lookup_context_t *context)
{
    return context && context->initialized && context->descriptors &&
           context->class_epochs;
}

static void mutation_lock(st_lookup_context_t *context)
{
    while (atomic_flag_test_and_set_explicit(&context->mutation_lock,
                                              memory_order_acquire)) {
    }
}

static void mutation_unlock(st_lookup_context_t *context)
{
    atomic_flag_clear_explicit(&context->mutation_lock, memory_order_release);
}

static bool class_is_descendant(const st_lookup_context_t *context,
                                uint32_t candidate_id,
                                uint32_t ancestor_id)
{
    size_t hops = 0;
    while (candidate_id != 0 &&
           hops++ < context->descriptors->class_count) {
        const StClassDescriptor *candidate = st_runtime_class(
            context->descriptors, candidate_id);
        if (!candidate) return false;
        if (candidate_id == ancestor_id) return true;
        candidate_id = candidate->superclass_id;
    }
    return false;
}

static StMethodEntry *class_method_entry(const StClassDescriptor *descriptor,
                                         st_selector_id_t selector_id)
{
    size_t low = 0;
    size_t high = descriptor->method_slot_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const st_method_slot_t *slot = &descriptor->method_slots[middle];
        if (slot->selector_id < selector_id)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == descriptor->method_slot_count ||
        descriptor->method_slots[low].selector_id != selector_id)
        return NULL;
    return descriptor->method_slots[low].entry;
}

static bool method_entries_are_canonical(
    const st_runtime_descriptors_t *descriptors)
{
    size_t class_index;
    for (class_index = 0; class_index < descriptors->class_count;
         class_index++) {
        const StClassDescriptor *descriptor = descriptors->classes[class_index];
        size_t slot_index;
        for (slot_index = 0; slot_index < descriptor->method_slot_count;
             slot_index++) {
            const st_method_slot_t *slot =
                &descriptor->method_slots[slot_index];
            const StClassDescriptor *owner = st_runtime_class(
                descriptors, slot->entry->owner_class_id);
            if (!owner || class_method_entry(owner, slot->selector_id) !=
                              slot->entry)
                return false;
        }
    }
    return true;
}

static bool entry_belongs_to_context(const st_lookup_context_t *context,
                                     const StMethodEntry *entry)
{
    const StClassDescriptor *owner;
    if (!entry) return false;
    owner = st_runtime_class(context->descriptors, entry->owner_class_id);
    return owner && class_method_entry(owner, entry->selector_id) == entry;
}

st_lookup_status_t st_lookup_context_init(
    st_lookup_context_t *context,
    const st_runtime_descriptors_t *descriptors,
    st_lookup_allocator_t allocator)
{
    return st_lookup_context_init_with_epoch(context, descriptors, allocator,
                                             ST_LOOKUP_INITIAL_EPOCH);
}

st_lookup_status_t st_lookup_context_init_with_epoch(
    st_lookup_context_t *context,
    const st_runtime_descriptors_t *descriptors,
    st_lookup_allocator_t allocator,
    uint32_t initial_epoch)
{
    _Atomic(uint32_t) *epochs;
    size_t bytes;
    size_t index;
    if (!context || context->initialized || !descriptors ||
        initial_epoch == 0 ||
        ((allocator.allocate == NULL) != (allocator.deallocate == NULL)))
        return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    if (st_runtime_descriptors_validate(descriptors) != ST_RUNTIME_OK)
        return ST_LOOKUP_ERR_INVALID_DESCRIPTOR;
    if (!method_entries_are_canonical(descriptors))
        return ST_LOOKUP_ERR_INVALID_DESCRIPTOR;
    if (descriptors->class_count > SIZE_MAX / sizeof(*epochs))
        return ST_LOOKUP_ERR_OUT_OF_MEMORY;
    if (!allocator.allocate) {
        allocator.allocate = default_allocate;
        allocator.deallocate = default_deallocate;
    }
    bytes = descriptors->class_count * sizeof(*epochs);
    epochs = allocator.allocate(allocator.user, bytes);
    if (!epochs) return ST_LOOKUP_ERR_OUT_OF_MEMORY;
    if (((uintptr_t)epochs % _Alignof(_Atomic(uint32_t))) != 0) {
        allocator.deallocate(allocator.user, epochs);
        return ST_LOOKUP_ERR_BAD_ALIGNMENT;
    }
    for (index = 0; index < descriptors->class_count; index++)
        atomic_init(&epochs[index], initial_epoch);
    context->descriptors = descriptors;
    context->class_epochs = epochs;
    context->allocator = allocator;
    atomic_flag_clear(&context->mutation_lock);
    context->initialized = true;
    return ST_LOOKUP_FOUND;
}

void st_lookup_context_destroy(st_lookup_context_t *context)
{
    if (!context) return;
    if (context->initialized && context->class_epochs &&
        context->allocator.deallocate)
        context->allocator.deallocate(context->allocator.user,
                                      context->class_epochs);
    memset(context, 0, sizeof(*context));
}

bool st_lookup_context_class_epoch(const st_lookup_context_t *context,
                                   uint32_t class_id,
                                   uint32_t *epoch_out)
{
    if (epoch_out) *epoch_out = 0;
    if (!context_is_valid(context) || !epoch_out || class_id == 0 ||
        (uint64_t)class_id > (uint64_t)context->descriptors->class_count)
        return false;
    *epoch_out = atomic_load_explicit(&context->class_epochs[class_id - 1],
                                      memory_order_acquire);
    return true;
}

static st_lookup_status_t invalidate_locked(st_lookup_context_t *context,
                                            uint32_t changed_class_id)
{
    size_t index;
    for (index = 0; index < context->descriptors->class_count; index++) {
        uint32_t class_id = (uint32_t)index + 1;
        if (class_is_descendant(context, class_id, changed_class_id) &&
            atomic_load_explicit(&context->class_epochs[index],
                                 memory_order_relaxed) == UINT32_MAX)
            return ST_LOOKUP_ERR_EPOCH_EXHAUSTED;
    }
    for (index = 0; index < context->descriptors->class_count; index++) {
        uint32_t class_id = (uint32_t)index + 1;
        if (class_is_descendant(context, class_id, changed_class_id))
            atomic_fetch_add_explicit(&context->class_epochs[index], 1,
                                      memory_order_release);
    }
    return ST_LOOKUP_FOUND;
}

st_lookup_status_t st_lookup_invalidate_class(st_lookup_context_t *context,
                                              uint32_t changed_class_id)
{
    st_lookup_status_t status;
    if (!context_is_valid(context) ||
        !st_runtime_class(context->descriptors, changed_class_id))
        return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    mutation_lock(context);
    status = invalidate_locked(context, changed_class_id);
    mutation_unlock(context);
    return status;
}

st_lookup_status_t st_lookup_publish_binding(
    st_lookup_context_t *context,
    StMethodEntry *entry,
    const StMethodBinding *new_binding,
    const StMethodBinding **old_binding_out)
{
    const StMethodBinding *old;
    st_lookup_status_t status;
    if (old_binding_out) *old_binding_out = NULL;
    if (!context_is_valid(context) || !entry ||
        !st_method_binding_is_valid(new_binding) ||
        new_binding->descriptor->selector_id != entry->selector_id ||
        new_binding->descriptor->owner_class_id != entry->owner_class_id ||
        !st_runtime_class(context->descriptors, entry->owner_class_id) ||
        !entry_belongs_to_context(context, entry))
        return ST_LOOKUP_ERR_INVALID_ARGUMENT;

    mutation_lock(context);
    /* Preflight epoch exhaustion before publishing: failure cannot leave a new
     * method visible behind an epoch which could not be advanced. */
    for (size_t index = 0; index < context->descriptors->class_count; index++) {
        uint32_t class_id = (uint32_t)index + 1;
        if (class_is_descendant(context, class_id, entry->owner_class_id) &&
            atomic_load_explicit(&context->class_epochs[index],
                                 memory_order_relaxed) == UINT32_MAX) {
            mutation_unlock(context);
            return ST_LOOKUP_ERR_EPOCH_EXHAUSTED;
        }
    }
    old = st_method_entry_load(entry);
    if (!old || new_binding->version <= old->version ||
        !st_method_entry_publish(entry, new_binding, NULL)) {
        mutation_unlock(context);
        return ST_LOOKUP_ERR_VERSION_CONFLICT;
    }
    status = invalidate_locked(context, entry->owner_class_id);
    mutation_unlock(context);
    if (status != ST_LOOKUP_FOUND) return status;
    if (old_binding_out) *old_binding_out = old;
    return ST_LOOKUP_FOUND;
}

st_lookup_status_t st_lookup_inherited(
    const st_lookup_context_t *context,
    uint32_t start_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out)
{
    uint32_t cursor = start_class_id;
    size_t hops = 0;
    clear_result(result_out);
    if (!context_is_valid(context) || !result_out || selector_id == 0 ||
        !st_runtime_class(context->descriptors, start_class_id))
        return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    while (cursor != 0 && hops++ < context->descriptors->class_count) {
        const StClassDescriptor *descriptor = st_runtime_class(
            context->descriptors, cursor);
        StMethodEntry *entry;
        const StMethodBinding *binding;
        if (!descriptor) return ST_LOOKUP_ERR_INVALID_DESCRIPTOR;
        entry = class_method_entry(descriptor, selector_id);
        if (entry) {
            binding = st_method_entry_load(entry);
            if (!binding || !st_method_binding_is_valid(binding) ||
                entry->selector_id != selector_id ||
                binding->descriptor->selector_id != selector_id ||
                entry->owner_class_id !=
                    binding->descriptor->owner_class_id)
                return ST_LOOKUP_ERR_INVALID_DESCRIPTOR;
            result_out->entry = entry;
            result_out->binding = binding;
            result_out->defining_class_id = entry->owner_class_id;
            return ST_LOOKUP_FOUND;
        }
        cursor = descriptor->superclass_id;
    }
    return cursor == 0 ? ST_LOOKUP_NOT_FOUND
                       : ST_LOOKUP_ERR_INVALID_DESCRIPTOR;
}

st_lookup_status_t st_lookup_super(
    const st_lookup_context_t *context,
    uint32_t lexical_owner_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out)
{
    const StClassDescriptor *owner;
    clear_result(result_out);
    if (!context_is_valid(context) || !result_out || selector_id == 0)
        return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    owner = st_runtime_class(context->descriptors, lexical_owner_class_id);
    if (!owner) return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    if (owner->superclass_id == 0) return ST_LOOKUP_NOT_FOUND;
    return st_lookup_inherited(context, owner->superclass_id, selector_id,
                               result_out);
}

st_lookup_status_t st_lookup_default_miss(
    void *user,
    const st_lookup_context_t *context,
    uint32_t receiver_class_id,
    uint32_t lookup_start_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out)
{
    (void)user;
    (void)receiver_class_id;
    if (lookup_start_class_id == 0) {
        clear_result(result_out);
        return ST_LOOKUP_NOT_FOUND;
    }
    return st_lookup_inherited(context, lookup_start_class_id, selector_id,
                               result_out);
}

static bool pic_slot_read(const st_pic_slot_t *slot, uint32_t *class_id_out,
                          uint32_t *epoch_out, StMethodEntry **entry_out,
                          const StMethodBinding **binding_out)
{
    uint64_t first;
    uint64_t second;
    first = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    if ((first & UINT64_C(1)) != 0) return false;
    *class_id_out = atomic_load_explicit(&slot->class_id,
                                         memory_order_relaxed);
    *epoch_out = atomic_load_explicit(&slot->epoch, memory_order_relaxed);
    *entry_out = atomic_load_explicit(&slot->entry, memory_order_relaxed);
    *binding_out = atomic_load_explicit(&slot->binding,
                                        memory_order_relaxed);
    second = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    return first == second && (second & UINT64_C(1)) == 0;
}

static bool pic_slot_write(st_pic_slot_t *slot, uint32_t class_id,
                           uint32_t epoch, StMethodEntry *entry,
                           const StMethodBinding *binding)
{
    uint64_t sequence = atomic_load_explicit(&slot->sequence,
                                             memory_order_acquire);
    for (;;) {
        if (sequence >= UINT64_MAX - UINT64_C(1)) return false;
        if ((sequence & UINT64_C(1)) != 0) {
            sequence = atomic_load_explicit(&slot->sequence,
                                            memory_order_acquire);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(
                &slot->sequence, &sequence, sequence + UINT64_C(1),
                memory_order_acq_rel, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&slot->class_id, class_id, memory_order_relaxed);
    atomic_store_explicit(&slot->epoch, epoch, memory_order_relaxed);
    atomic_store_explicit(&slot->entry, entry, memory_order_relaxed);
    atomic_store_explicit(&slot->binding, binding, memory_order_relaxed);
    atomic_store_explicit(&slot->sequence, sequence + UINT64_C(2),
                          memory_order_release);
    return true;
}

bool st_send_site_init(st_send_site_t *site, st_selector_id_t selector_id,
                       uint32_t lexical_owner_class_id)
{
    size_t index;
    if (!site || site->initialized || selector_id == 0) return false;
    memset(site, 0, sizeof(*site));
    site->selector_id = selector_id;
    site->lexical_owner_class_id = lexical_owner_class_id;
    atomic_init(&site->next_victim, 0);
    for (index = 0; index < ST_PIC_WAYS; index++) {
        atomic_init(&site->slots[index].sequence, 0);
        atomic_init(&site->slots[index].class_id, 0);
        atomic_init(&site->slots[index].epoch, 0);
        atomic_init(&site->slots[index].entry, NULL);
        atomic_init(&site->slots[index].binding, NULL);
    }
    site->initialized = true;
    return true;
}

void st_send_site_clear(st_send_site_t *site)
{
    size_t index;
    if (!site || !site->initialized) return;
    for (index = 0; index < ST_PIC_WAYS; index++)
        (void)pic_slot_write(&site->slots[index], 0, 0, NULL, NULL);
}

static bool lookup_result_is_well_formed(
    const st_lookup_context_t *context,
    uint32_t lookup_start_class_id,
    st_selector_id_t selector_id,
    const st_lookup_result_t *result)
{
    if (!result || !result->entry || !result->binding ||
        result->defining_class_id == 0 ||
        result->entry->selector_id != selector_id ||
        result->entry->owner_class_id != result->defining_class_id ||
        !st_method_binding_is_valid(result->binding) ||
        result->binding->descriptor->selector_id != selector_id ||
        result->binding->descriptor->owner_class_id !=
            result->defining_class_id ||
        !entry_belongs_to_context(context, result->entry) ||
        !class_is_descendant(context, lookup_start_class_id,
                             result->defining_class_id))
        return false;
    return true;
}

static void pic_store(st_send_site_t *site, uint32_t receiver_class_id,
                      uint32_t epoch, const st_lookup_result_t *result)
{
    size_t index;
    for (index = 0; index < ST_PIC_WAYS; index++) {
        uint32_t class_id;
        uint32_t ignored_epoch;
        StMethodEntry *ignored_entry;
        const StMethodBinding *ignored_binding;
        if (pic_slot_read(&site->slots[index], &class_id, &ignored_epoch,
                          &ignored_entry, &ignored_binding) &&
            class_id == receiver_class_id) {
            (void)pic_slot_write(&site->slots[index], receiver_class_id,
                                 epoch, result->entry, result->binding);
            return;
        }
    }
    index = atomic_fetch_add_explicit(&site->next_victim, 1,
                                      memory_order_relaxed) &
            (ST_PIC_WAYS - 1u);
    (void)pic_slot_write(&site->slots[index], receiver_class_id, epoch,
                         result->entry, result->binding);
}

st_lookup_status_t st_send_site_resolve(
    const st_lookup_context_t *context,
    st_send_site_t *site,
    uint32_t receiver_class_id,
    st_lookup_miss_fn miss,
    void *miss_user,
    st_lookup_result_t *result_out,
    bool *cache_hit_out)
{
    const StClassDescriptor *receiver;
    uint32_t lookup_start_class_id;
    uint32_t epoch;
    size_t index;
    st_lookup_status_t status;
    clear_result(result_out);
    if (cache_hit_out) *cache_hit_out = false;
    if (!context_is_valid(context) || !site || !site->initialized ||
        !result_out || !cache_hit_out)
        return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    receiver = st_runtime_class(context->descriptors, receiver_class_id);
    if (!receiver) return ST_LOOKUP_ERR_INVALID_ARGUMENT;
    if (site->lexical_owner_class_id == 0) {
        lookup_start_class_id = receiver_class_id;
    } else {
        const StClassDescriptor *owner = st_runtime_class(
            context->descriptors, site->lexical_owner_class_id);
        if (!owner || !class_is_descendant(context, receiver_class_id,
                                            owner->class_id))
            return ST_LOOKUP_ERR_INVALID_ARGUMENT;
        lookup_start_class_id = owner->superclass_id;
    }
    epoch = atomic_load_explicit(
        &context->class_epochs[receiver_class_id - 1], memory_order_acquire);
    for (index = 0; index < ST_PIC_WAYS; index++) {
        uint32_t cached_class_id;
        uint32_t cached_epoch;
        StMethodEntry *cached_entry;
        const StMethodBinding *cached_binding;
        uint32_t confirmed_epoch;
        if (!pic_slot_read(&site->slots[index], &cached_class_id,
                           &cached_epoch, &cached_entry, &cached_binding) ||
            cached_class_id != receiver_class_id || cached_epoch != epoch ||
            !cached_entry || !cached_binding ||
            st_method_entry_load(cached_entry) != cached_binding)
            continue;
        confirmed_epoch = atomic_load_explicit(
            &context->class_epochs[receiver_class_id - 1],
            memory_order_acquire);
        if (confirmed_epoch != epoch) break;
        result_out->entry = cached_entry;
        result_out->binding = cached_binding;
        result_out->defining_class_id = cached_entry->owner_class_id;
        *cache_hit_out = true;
        return ST_LOOKUP_FOUND;
    }

    if (!miss) miss = st_lookup_default_miss;
    status = miss(miss_user, context, receiver_class_id,
                  lookup_start_class_id, site->selector_id, result_out);
    if (status == ST_LOOKUP_NOT_FOUND) {
        if (result_out->entry || result_out->binding ||
            result_out->defining_class_id != 0) {
            clear_result(result_out);
            return ST_LOOKUP_ERR_CALLBACK_RESULT;
        }
        return status;
    }
    if (status != ST_LOOKUP_FOUND) {
        clear_result(result_out);
        return status;
    }
    if (lookup_start_class_id == 0 ||
        !lookup_result_is_well_formed(context, lookup_start_class_id,
                                      site->selector_id, result_out)) {
        clear_result(result_out);
        return ST_LOOKUP_ERR_CALLBACK_RESULT;
    }
    /* A concurrent replacement may supersede the immutable snapshot after the
     * callback observed it.  That snapshot is still a valid linearizable
     * result, but it must not be installed into the PIC under a newer epoch. */
    if (st_method_entry_load(result_out->entry) == result_out->binding) {
        epoch = atomic_load_explicit(
            &context->class_epochs[receiver_class_id - 1],
            memory_order_acquire);
        pic_store(site, receiver_class_id, epoch, result_out);
    }
    return ST_LOOKUP_FOUND;
}

const char *st_lookup_status_string(st_lookup_status_t status)
{
    switch (status) {
    case ST_LOOKUP_FOUND: return "found";
    case ST_LOOKUP_NOT_FOUND: return "not found";
    case ST_LOOKUP_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_LOOKUP_ERR_INVALID_DESCRIPTOR: return "invalid descriptor";
    case ST_LOOKUP_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_LOOKUP_ERR_BAD_ALIGNMENT: return "bad alignment";
    case ST_LOOKUP_ERR_EPOCH_EXHAUSTED: return "epoch exhausted";
    case ST_LOOKUP_ERR_VERSION_CONFLICT: return "version conflict";
    case ST_LOOKUP_ERR_CALLBACK_RESULT: return "invalid miss callback result";
    default: return "unknown lookup status";
    }
}
