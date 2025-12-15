/*
 * MCC Standard Library - setjmp.h
 * Non-local jumps
 */

#ifndef _SETJMP_H
#define _SETJMP_H

/* Jump buffer type - architecture dependent */
typedef int jmp_buf[16];

/* Save calling environment */
extern int setjmp(jmp_buf env);

/* Restore calling environment */
extern void longjmp(jmp_buf env, int val);

#endif /* _SETJMP_H */
