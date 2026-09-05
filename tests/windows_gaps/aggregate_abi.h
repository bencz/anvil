#ifndef ANVIL_TEST_AGGREGATE_ABI_H
#define ANVIL_TEST_AGGREGATE_ABI_H

struct Byte { unsigned char value; };
struct Word { unsigned short value; };
struct Odd { unsigned char bytes[3]; };
struct Small { int value; };
struct Eight { unsigned char bytes[8]; };
struct Pair { double left; double right; };
struct Large { long long a; long long b; long long c; };
struct Buffer { unsigned char bytes[83]; };
union Scalar { double real; unsigned long long bits; };

struct Byte change_byte(struct Byte value);
struct Word change_word(struct Word value);
struct Odd change_odd(int prefix, struct Odd value, double scale, int tail);
struct Small change_small(struct Small value);
struct Eight change_eight(struct Eight value);
struct Pair change_pair(int prefix, struct Pair value, double scale, int tail);
struct Large change_large(struct Large value, int b, int c, int d, int e);
union Scalar change_scalar(union Scalar value);
struct Buffer change_buffer(struct Buffer value);

#endif
