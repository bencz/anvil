/*
 * MCC Standard Library - time.h
 * Date and time
 */

#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

/* Clock ticks per second */
#define CLOCKS_PER_SEC  1000000L

/* Clock type */
typedef long clock_t;

/* Time type */
typedef long time_t;

/* Time structure */
struct tm {
    int tm_sec;     /* Seconds (0-60) */
    int tm_min;     /* Minutes (0-59) */
    int tm_hour;    /* Hours (0-23) */
    int tm_mday;    /* Day of month (1-31) */
    int tm_mon;     /* Month (0-11) */
    int tm_year;    /* Year since 1900 */
    int tm_wday;    /* Day of week (0-6, Sunday = 0) */
    int tm_yday;    /* Day of year (0-365) */
    int tm_isdst;   /* Daylight saving time flag */
};

/* Time manipulation functions */
extern clock_t clock(void);
extern double difftime(time_t time1, time_t time0);
extern time_t mktime(struct tm *timeptr);
extern time_t time(time_t *timer);

/* Time conversion functions */
extern char *asctime(const struct tm *timeptr);
extern char *ctime(const time_t *timer);
extern struct tm *gmtime(const time_t *timer);
extern struct tm *localtime(const time_t *timer);
extern size_t strftime(char *s, size_t maxsize, const char *format,
                       const struct tm *timeptr);

#endif /* _TIME_H */
