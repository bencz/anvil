/*
 * C89 Test: Preprocessor Include
 * Tests #include directive and include guards
 */

/* Include guard pattern */
#ifndef PP_INCLUDE_TEST_H
#define PP_INCLUDE_TEST_H

/* Simulated header content */
#define HEADER_VERSION 1
#define HEADER_LOADED 1

typedef int my_int_t;

#endif /* PP_INCLUDE_TEST_H */

/* Second include should be ignored due to guard */
#ifndef PP_INCLUDE_TEST_H
#define PP_INCLUDE_TEST_H
#error "Include guard failed!"
#endif

/* Nested include guard simulation */
#ifndef NESTED_A_H
#define NESTED_A_H

#ifndef NESTED_B_H
#define NESTED_B_H
#define NESTED_VALUE 42
#endif /* NESTED_B_H */

#endif /* NESTED_A_H */

/* Pragma once simulation (using guards) */
#ifndef PRAGMA_ONCE_SIM_H
#define PRAGMA_ONCE_SIM_H
#define PRAGMA_ONCE_LOADED 1
#endif

int main(void)
{
    my_int_t x = HEADER_VERSION;
    int y = NESTED_VALUE;
    int z = PRAGMA_ONCE_LOADED;
    
    return x + y + z;
}
