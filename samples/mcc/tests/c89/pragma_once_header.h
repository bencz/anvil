#pragma once

#ifdef MCC_PRAGMA_ONCE_HEADER_SEEN
#error pragma once failed to suppress a repeated include
#endif
#define MCC_PRAGMA_ONCE_HEADER_SEEN 1

struct pragma_once_record {
    int value;
};
