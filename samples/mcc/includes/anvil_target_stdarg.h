#ifndef MCC_TARGET_STDARG_H
#define MCC_TARGET_STDARG_H

typedef char *va_list;
char *__anvil_va_start(void);
char *__anvil_va_copy(char *cursor);
void *__anvil_va_arg(char **cursor, size_t type_size);

#define va_start(ap, last) ((ap) = __anvil_va_start())
#define va_arg(ap, type) (*(type *)__anvil_va_arg(&(ap), sizeof(type)))
#define va_end(ap) ((ap) = (va_list)0)
#define va_copy(dest, src) ((dest) = __anvil_va_copy(src))

#endif
