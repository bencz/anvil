/*
 * MCC Standard Library - signal.h
 * Signal handling
 */

#ifndef _SIGNAL_H
#define _SIGNAL_H

/* Signal handler type */
typedef void (*sig_t)(int);

/* Signal handler return values */
#define SIG_DFL ((sig_t)0)      /* Default action */
#define SIG_IGN ((sig_t)1)      /* Ignore signal */
#define SIG_ERR ((sig_t)-1)     /* Error return */

/* Signal numbers */
#define SIGABRT     6       /* Abnormal termination */
#define SIGFPE      8       /* Floating-point exception */
#define SIGILL      4       /* Illegal instruction */
#define SIGINT      2       /* Interactive attention signal */
#define SIGSEGV     11      /* Segmentation violation */
#define SIGTERM     15      /* Termination request */

/* Signal type for sig_atomic_t */
typedef int sig_atomic_t;

/* Signal functions */
extern sig_t signal(int sig, sig_t handler);
extern int raise(int sig);

#endif /* _SIGNAL_H */
