#ifndef ANVIL_SMALLTALK_LOOKUP_H
#define ANVIL_SMALLTALK_LOOKUP_H

#include "st_runtime.h"
#include "st_selector.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_PIC_WAYS 4u
#define ST_LOOKUP_INITIAL_EPOCH UINT32_C(1)

typedef enum {
    ST_LOOKUP_FOUND = 0,
    ST_LOOKUP_NOT_FOUND,
    ST_LOOKUP_ERR_INVALID_ARGUMENT,
    ST_LOOKUP_ERR_INVALID_DESCRIPTOR,
    ST_LOOKUP_ERR_OUT_OF_MEMORY,
    ST_LOOKUP_ERR_BAD_ALIGNMENT,
    ST_LOOKUP_ERR_EPOCH_EXHAUSTED,
    ST_LOOKUP_ERR_VERSION_CONFLICT,
    ST_LOOKUP_ERR_CALLBACK_RESULT
} st_lookup_status_t;

typedef void *(*st_lookup_allocate_fn)(void *user, size_t size);
typedef void (*st_lookup_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_lookup_allocate_fn allocate;
    st_lookup_deallocate_fn deallocate;
    void *user;
} st_lookup_allocator_t;

/* A successful lookup returns both the stable entry and the immutable binding
 * snapshot observed through that entry.  Binding storage must obey the
 * image-lifetime (or an equivalent safe-reclamation) rule in st_runtime.h. */
typedef struct {
    StMethodEntry *entry;
    const StMethodBinding *binding;
    uint32_t defining_class_id;
} st_lookup_result_t;

typedef struct {
    const st_runtime_descriptors_t *descriptors;
    _Atomic(uint32_t) *class_epochs;
    st_lookup_allocator_t allocator;
    atomic_flag mutation_lock;
    bool initialized;
} st_lookup_context_t;

/* `context` must be zero-initialized or previously destroyed.  The descriptor
 * graph and all method slot arrays are validated and thereafter treated as
 * immutable.  Epochs are kept separately because StClassDescriptor is AOT ABI
 * data and deliberately has no mutable runtime fields.  Descriptor and binding
 * storage must outlive the context; destroy must not race with lookup. */
st_lookup_status_t st_lookup_context_init(
    st_lookup_context_t *context,
    const st_runtime_descriptors_t *descriptors,
    st_lookup_allocator_t allocator);

/* Intended for restoring a validated snapshot and for wrap-policy testing.
 * New contexts should normally use st_lookup_context_init().  Epoch zero is
 * reserved and is rejected. */
st_lookup_status_t st_lookup_context_init_with_epoch(
    st_lookup_context_t *context,
    const st_runtime_descriptors_t *descriptors,
    st_lookup_allocator_t allocator,
    uint32_t initial_epoch);

void st_lookup_context_destroy(st_lookup_context_t *context);
bool st_lookup_context_class_epoch(const st_lookup_context_t *context,
                                   uint32_t class_id,
                                   uint32_t *epoch_out);

/* Invalidating a class also invalidates every descendant, because an inherited
 * lookup result depends on every ancestor in its lookup chain.  Epochs never
 * wrap: if any affected epoch is UINT32_MAX the operation is transactional and
 * returns ST_LOOKUP_ERR_EPOCH_EXHAUSTED without changing any epoch. */
st_lookup_status_t st_lookup_invalidate_class(st_lookup_context_t *context,
                                              uint32_t changed_class_id);

/* Publish a strictly newer immutable binding and invalidate the owner's entire
 * descendant cone under the context's mutation lock.  The entry address never
 * changes.  Direct st_method_entry_publish() remains safe: PIC hits also verify
 * the currently published binding pointer, though callers then forgo eager
 * descendant invalidation. */
st_lookup_status_t st_lookup_publish_binding(
    st_lookup_context_t *context,
    StMethodEntry *entry,
    const StMethodBinding *new_binding,
    const StMethodBinding **old_binding_out);

/* Search `start_class_id`, then its superclasses.  Each class table is searched
 * by selector ID using binary search.  NOT_FOUND is an explicit result and is
 * never converted into nil or a placeholder doesNotUnderstand: method. */
st_lookup_status_t st_lookup_inherited(
    const st_lookup_context_t *context,
    uint32_t start_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out);

/* Lexical super lookup begins at the superclass of `lexical_owner_class_id`,
 * independent of the receiver's dynamic class. */
st_lookup_status_t st_lookup_super(
    const st_lookup_context_t *context,
    uint32_t lexical_owner_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out);

typedef st_lookup_status_t (*st_lookup_miss_fn)(
    void *user,
    const st_lookup_context_t *context,
    uint32_t receiver_class_id,
    uint32_t lookup_start_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out);

/* Default generic miss path: perform the ordinary inherited lookup selected by
 * `lookup_start_class_id`.  A VM may provide an equivalent instrumented or
 * policy-aware callback, but its result is validated before it enters a PIC. */
st_lookup_status_t st_lookup_default_miss(
    void *user,
    const st_lookup_context_t *context,
    uint32_t receiver_class_id,
    uint32_t lookup_start_class_id,
    st_selector_id_t selector_id,
    st_lookup_result_t *result_out);

typedef struct {
    _Atomic(uint64_t) sequence;
    _Atomic(uint32_t) class_id;
    _Atomic(uint32_t) epoch;
    _Atomic(StMethodEntry *) entry;
    _Atomic(const StMethodBinding *) binding;
} st_pic_slot_t;

typedef struct {
    st_selector_id_t selector_id;
    /* Zero means normal lookup from the receiver class.  Nonzero identifies
     * the lexical method owner for a super send. */
    uint32_t lexical_owner_class_id;
    _Atomic(uint32_t) next_victim;
    st_pic_slot_t slots[ST_PIC_WAYS];
    bool initialized;
} st_send_site_t;

/* `site` must be zero-initialized.  The first populated way is the monomorphic
 * state; distinct receiver classes grow it naturally to the four-way PIC. */
bool st_send_site_init(st_send_site_t *site, st_selector_id_t selector_id,
                       uint32_t lexical_owner_class_id);

/* Clear only cached data.  The selector and lexical-super policy remain
 * unchanged.  Clearing and resolving may execute concurrently. */
void st_send_site_clear(st_send_site_t *site);

/* Resolve through a coherent four-way polymorphic inline cache.  PIC storage
 * is data-only; generated AOT code may call this function or inline the same
 * atomic probe protocol, but the runtime never emits or patches code. */
st_lookup_status_t st_send_site_resolve(
    const st_lookup_context_t *context,
    st_send_site_t *site,
    uint32_t receiver_class_id,
    st_lookup_miss_fn miss,
    void *miss_user,
    st_lookup_result_t *result_out,
    bool *cache_hit_out);

const char *st_lookup_status_string(st_lookup_status_t status);

#ifdef __cplusplus
}
#endif

#endif
