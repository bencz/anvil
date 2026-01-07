cmake_minimum_required(VERSION 3.10)
project(anvil C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ANVIL_BUILD_EXAMPLES "Build example programs" ON)
option(ANVIL_BUILD_TESTS "Build test programs" ON)

include_directories(include)
include_directories(src)

set(ANVIL_CORE_SOURCES
    src/core/arena.c
    src/core/vec.c
    src/core/str.c
    src/core/hash.c
    src/core/error.c
)

set(ANVIL_IR_SOURCES
    src/ir/types.c
    src/ir/value.c
    src/ir/inst.c
    src/ir/func.c
    src/ir/builder.c
    src/ir/module.c
)

set(ANVIL_MIR_SOURCES
    src/mir/mir.c
    src/mir/lower.c
    src/mir/cfg.c
    src/mir/liveness.c
    src/mir/regalloc.c
    src/mir/isel.c
)

set(ANVIL_OPT_SOURCES
    src/opt/ir_opt.c
    src/opt/mir_opt.c
)

set(ANVIL_BACKEND_SOURCES
    src/backend/backend.c
    src/backend/x86_64/target.c
    src/backend/x86_64/regs.c
    src/backend/x86_64/emit.c
    src/backend/x86_64/x86_64.c
    src/backend/x86_64/abi/sysv.c
    src/backend/x86_64/abi/win64.c
    src/backend/x86_64/opt/peephole.c
    src/backend/x86_64/isel/isel.c
    src/backend/arm64/target.c
    src/backend/arm64/regs.c
    src/backend/arm64/emit.c
    src/backend/arm64/arm64.c
    src/backend/arm64/abi/aapcs64.c
    src/backend/arm64/abi/apple.c
    src/backend/arm64/opt/peephole.c
    src/backend/arm64/isel/isel.c
    src/backend/ppc64/target.c
    src/backend/ppc64/regs.c
    src/backend/ppc64/emit.c
    src/backend/ppc64/ppc64.c
    src/backend/ppc64/abi/elfv2.c
    src/backend/ppc64/opt/peephole.c
    src/backend/ppc64/isel/isel.c
)

set(ANVIL_API_SOURCES
    src/api.c
)

add_library(anvil STATIC
    ${ANVIL_CORE_SOURCES}
    ${ANVIL_IR_SOURCES}
    ${ANVIL_MIR_SOURCES}
    ${ANVIL_OPT_SOURCES}
    ${ANVIL_BACKEND_SOURCES}
    ${ANVIL_API_SOURCES}
)

target_include_directories(anvil PUBLIC include)
target_include_directories(anvil PRIVATE src)

if(ANVIL_BUILD_EXAMPLES)
    add_executable(hello examples/hello.c)
    target_link_libraries(hello anvil)
    
    add_executable(fibonacci examples/fibonacci.c)
    target_link_libraries(fibonacci anvil)
    
    add_executable(sum_array examples/sum_array.c)
    target_link_libraries(sum_array anvil)
    
    add_executable(multi_target examples/multi_target.c)
    target_link_libraries(multi_target anvil)
    
    add_executable(main_with_printf examples/main_with_printf.c)
    target_link_libraries(main_with_printf anvil)
    
    add_executable(printf_many_args examples/printf_many_args.c)
    target_link_libraries(printf_many_args anvil)

    add_executable(ppc64_test examples/ppc64_test.c)
    target_link_libraries(ppc64_test anvil)
    
    add_executable(test_compile examples/test_compile.c)
    target_link_libraries(test_compile anvil)
endif()

if(ANVIL_BUILD_TESTS)
    enable_testing()
    
    add_executable(test_arena tests/unit/test_arena.c)
    target_link_libraries(test_arena anvil)
    add_test(NAME test_arena COMMAND test_arena)
    
    add_executable(test_types tests/unit/test_types.c)
    target_link_libraries(test_types anvil)
    add_test(NAME test_types COMMAND test_types)
endif()

install(TARGETS anvil
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES include/anvil.h DESTINATION include)
