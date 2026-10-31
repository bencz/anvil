#ifndef MCC_TARGET_STDARG_H
#define MCC_TARGET_STDARG_H

/* 32-bit PowerPC SVR4: register counters, the overflow (stack) area and
 * the register save area the callee fills. */
typedef struct {
    unsigned char gpr;
    unsigned char fpr;
    unsigned short reserved;
    void *overflow_arg_area;
    void *reg_save_area;
} va_list[1];

char *__anvil_va_start_into(void *destination);
char *__anvil_va_copy_into(void *destination, const void *source);
void *__anvil_va_arg_at(void *cursor, size_t type_size);

#define va_start(ap, last) ((void)__anvil_va_start_into(ap))
#define va_arg(ap, type) (*(type *)__anvil_va_arg_at(ap, sizeof(type)))
#define va_copy(dest, src) ((void)__anvil_va_copy_into(dest, src))
#define va_end(ap) ((void)0)

#endif
