#ifndef ANVIL_TEST_THREADS_H
#define ANVIL_TEST_THREADS_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*anvil_test_thread_fn)(void *argument, size_t index);
bool anvil_test_run_threads(size_t count, anvil_test_thread_fn function, void *argument);

#endif
