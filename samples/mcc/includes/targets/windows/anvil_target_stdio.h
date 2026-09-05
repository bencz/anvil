#ifndef MCC_TARGET_STDIO_H
#define MCC_TARGET_STDIO_H

extern FILE *__acrt_iob_func(unsigned index);

#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))

#endif
