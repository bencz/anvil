#ifndef MCC_TARGET_STDARG_H
#define MCC_TARGET_STDARG_H

typedef struct {
    void *__stack;
    void *__gr_top;
    void *__vr_top;
    int __gr_offs;
    int __vr_offs;
} va_list;

char *__anvil_va_start_into(void *destination);
char *__anvil_va_copy_into(void *destination, const void *source);
void *__anvil_va_arg_at(void *cursor, size_t type_size);

#define va_start(ap, last) ((void)__anvil_va_start_into(&(ap)))
#define va_arg(ap, type) (*(type *)__anvil_va_arg_at(&(ap), sizeof(type)))
#define va_copy(dest, src) ((void)__anvil_va_copy_into(&(dest), &(src)))
#define va_end(ap) ((void)0)

#endif
