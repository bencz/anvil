#ifndef ANVIL_SMALLTALK_FIBER_IO_H
#define ANVIL_SMALLTALK_FIBER_IO_H

#include "st_fiber.h"
#include "../platform/socket.h"

/* prepare roots the entire activation before an overlapped operation borrows
 * its buffer. finish is mandatory after completion/cancellation has drained.
 * A deadline is an absolute monotonic millisecond count; UINT64_MAX has none. */
st_fiber_status_t st_fiber_io_prepare(StFrame *frame, st_socket_reactor_t **reactor_out);
st_fiber_status_t st_fiber_io_validate_frame(StFrame *frame);
st_fiber_status_t st_fiber_io_wait(StFrame *frame, uintptr_t socket, uint32_t events, uint64_t deadline, int *error_out);
void st_fiber_io_finish(StFrame *frame);
void st_fiber_io_interrupt(StFrame *frame, uintptr_t socket);
uint64_t st_fiber_milliseconds(void);

#endif
