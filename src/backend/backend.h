#ifndef ANVIL_BACKEND_H
#define ANVIL_BACKEND_H

#include "../mir/mir.h"
#include "../core/str.h"
#include "../../include/anvil/target.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnvilEndian {
    ANVIL_ENDIAN_LITTLE,
    ANVIL_ENDIAN_BIG,
    ANVIL_ENDIAN_MIXED,
} AnvilEndian;

typedef enum AnvilStackDir {
    ANVIL_STACK_GROWS_DOWN,
    ANVIL_STACK_GROWS_UP,
} AnvilStackDir;

typedef enum AnvilFloatFormat {
    ANVIL_FLOAT_IEEE754,
    ANVIL_FLOAT_IBM_HEX,
    ANVIL_FLOAT_VAX,
    ANVIL_FLOAT_NONE,
} AnvilFloatFormat;

typedef struct AnvilTargetFeatures {
    bool has_fpu;
    bool has_simd;
    bool has_atomic;
    bool has_unaligned_access;
    bool has_div;
    bool has_mul;
    bool has_barrel_shifter;
    bool has_conditional_move;
    bool has_branch_delay_slot;
} AnvilTargetFeatures;

typedef struct AnvilTargetInfo {
    const char* name;
    
    uint8_t word_size;
    uint8_t ptr_size;
    uint8_t min_align;
    uint8_t max_align;
    
    uint8_t sizeof_bool;
    uint8_t sizeof_char;
    uint8_t sizeof_short;
    uint8_t sizeof_int;
    uint8_t sizeof_long;
    uint8_t sizeof_longlong;
    uint8_t sizeof_float;
    uint8_t sizeof_double;
    uint8_t sizeof_longdouble;
    uint8_t sizeof_ptr;
    
    uint8_t alignof_bool;
    uint8_t alignof_char;
    uint8_t alignof_short;
    uint8_t alignof_int;
    uint8_t alignof_long;
    uint8_t alignof_longlong;
    uint8_t alignof_float;
    uint8_t alignof_double;
    uint8_t alignof_ptr;
    uint8_t alignof_stack;
    
    AnvilEndian endianness;
    AnvilStackDir stack_direction;
    AnvilFloatFormat float_format;
    AnvilTargetFeatures features;
    
    int max_imm_bits;
    int max_displacement;
} AnvilTargetInfo;

typedef enum AnvilRegClass {
    ANVIL_REG_CLASS_GP,
    ANVIL_REG_CLASS_FP,
    ANVIL_REG_CLASS_SIMD,
    ANVIL_REG_CLASS_SPECIAL,
} AnvilRegClass;

typedef struct AnvilRegInfo {
    const char* name;
    int id;
    int size_bits;
    AnvilRegClass reg_class;
    int parent_reg;
    int subreg_offset;
    int subreg_size;
    int dwarf_number;
} AnvilRegInfo;

typedef struct AnvilRegSet {
    const char* name;
    const AnvilRegInfo* regs;
    int num_regs;
    
    const int* gp_regs;
    int num_gp_regs;
    const int* fp_regs;
    int num_fp_regs;
    const int* simd_regs;
    int num_simd_regs;
    
    int stack_pointer;
    int frame_pointer;
    int link_register;
    int program_counter;
    int flags_register;
    
    const int* scratch_regs;
    int num_scratch_regs;
} AnvilRegSet;

typedef enum AnvilArgClass {
    ANVIL_ARG_CLASS_INTEGER,
    ANVIL_ARG_CLASS_SSE,
    ANVIL_ARG_CLASS_SSEUP,
    ANVIL_ARG_CLASS_X87,
    ANVIL_ARG_CLASS_MEMORY,
    ANVIL_ARG_CLASS_COMPLEX_X87,
} AnvilArgClass;

typedef struct AnvilArgInfo {
    AnvilArgClass arg_class;
    int reg;
    int stack_offset;
    int size;
    int align;
} AnvilArgInfo;

typedef struct AnvilFrameLayout {
    int local_size;
    int spill_size;
    int outgoing_args_size;
    int saved_regs_size;
    int total_size;
    int frame_pointer_offset;
} AnvilFrameLayout;

struct AnvilABI;
struct AnvilBackend;
struct AnvilAsmBuffer;

typedef void (*AnvilClassifyArgFn)(const struct AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, int arg_index, AnvilArgInfo* out);
typedef void (*AnvilClassifyRetFn)(const struct AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, AnvilArgInfo* out);
typedef void (*AnvilComputeFrameFn)(const struct AnvilABI* abi, const AnvilTargetInfo* target,
                                     AnvilMFunc* func, AnvilFrameLayout* out);
typedef const char* (*AnvilFormatSymbolFn)(const struct AnvilABI* abi, const char* name, 
                                            char* buffer, size_t buffer_size);
typedef void (*AnvilEmitCallFn)(const struct AnvilABI* abi, struct AnvilBackend* backend,
                                 const char* func_name, int num_args, AnvilMOperand* args,
                                 AnvilMOperand* ret, struct AnvilAsmBuffer* out);
typedef void (*AnvilEmitStringFn)(const struct AnvilABI* abi, const char* label, 
                                   const char* value, struct AnvilAsmBuffer* out);
typedef bool (*AnvilIsVariadicFn)(const struct AnvilABI* abi, const char* func_name);
typedef void (*AnvilEmitVariadicArgFn)(const struct AnvilABI* abi, int arg_index,
                                        AnvilMOperand* arg, struct AnvilAsmBuffer* out);

typedef struct AnvilABI {
    const char* name;
    
    const int* arg_regs_int;
    int num_arg_regs_int;
    const int* arg_regs_float;
    int num_arg_regs_float;
    
    int ret_reg_int_lo;
    int ret_reg_int_hi;
    int ret_reg_float;
    
    const int* callee_saved_regs;
    int num_callee_saved;
    const int* caller_saved_regs;
    int num_caller_saved;
    
    int stack_alignment;
    int arg_area_alignment;
    int red_zone_size;
    
    bool args_right_to_left;
    bool callee_cleans_stack;
    bool return_in_memory_hidden_arg;
    
    AnvilClassifyArgFn classify_argument;
    AnvilClassifyRetFn classify_return;
    AnvilComputeFrameFn compute_frame_layout;
    AnvilFormatSymbolFn format_symbol;
    AnvilEmitCallFn emit_call;
    AnvilEmitStringFn emit_string;
    AnvilIsVariadicFn is_variadic;
    AnvilEmitVariadicArgFn emit_variadic_arg;
    
    bool uses_underscore_prefix;
    bool variadic_args_on_stack;
} AnvilABI;

typedef struct AnvilAsmBuffer {
    AnvilStrBuilder sb;
} AnvilAsmBuffer;

void anvil_asm_init(AnvilAsmBuffer* buf);
void anvil_asm_free(AnvilAsmBuffer* buf);
void anvil_asm_append(AnvilAsmBuffer* buf, const char* fmt, ...);
void anvil_asm_newline(AnvilAsmBuffer* buf);
char* anvil_asm_take(AnvilAsmBuffer* buf);

typedef void (*AnvilLowerMirFn)(struct AnvilBackend* backend, AnvilMIR* mir);
typedef void (*AnvilEmitMirFn)(struct AnvilBackend* backend, AnvilMIR* mir, AnvilAsmBuffer* out, int os, const char* abi_name);
typedef void (*AnvilSelectInstFn)(struct AnvilBackend* backend, AnvilMInst* inst, AnvilVec* output);
typedef void (*AnvilRegAllocFn)(struct AnvilBackend* backend, AnvilMFunc* func, int os, const char* abi_name);
typedef int (*AnvilSpillCostFn)(struct AnvilBackend* backend, int reg);
typedef void (*AnvilEmitPrologueFn)(struct AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);
typedef void (*AnvilEmitEpilogueFn)(struct AnvilBackend* backend, AnvilMFunc* func, AnvilAsmBuffer* out);
typedef void (*AnvilEmitInstFn)(struct AnvilBackend* backend, AnvilMInst* inst, AnvilAsmBuffer* out);
typedef void (*AnvilEmitLabelFn)(struct AnvilBackend* backend, const char* label, AnvilAsmBuffer* out);
typedef void (*AnvilEmitDataFn)(struct AnvilBackend* backend, void* data, AnvilAsmBuffer* out);
typedef void (*AnvilPeepholeFn)(struct AnvilBackend* backend, AnvilMFunc* func);
typedef void (*AnvilScheduleFn)(struct AnvilBackend* backend, AnvilMBlock* block);
typedef void (*AnvilVectorizeFn)(struct AnvilBackend* backend, AnvilMFunc* func);
typedef void (*AnvilISelFn)(struct AnvilBackend* backend, AnvilMFunc* func);
typedef const char* (*AnvilRegNameFn)(struct AnvilBackend* backend, int reg_id, int size_bits);
typedef bool (*AnvilImmFitsFn)(struct AnvilBackend* backend, int64_t value, AnvilMInstKind kind);
typedef void (*AnvilMaterializeConstFn)(struct AnvilBackend* backend, int64_t value, int dest_reg, AnvilVec* output);

typedef const AnvilABI* (*AnvilGetAbiFn)(int os, const char* abi_name);

typedef struct AnvilBackend {
    const char* name;
    int word_size;
    AnvilEndian endianness;
    
    const AnvilTargetInfo* target_info;
    const AnvilRegSet* reg_set;
    
    const AnvilABI* default_abi;
    const AnvilABI** supported_abis;
    int num_supported_abis;
    
    AnvilLowerMirFn lower_mir;
    AnvilEmitMirFn emit_mir;
    AnvilRegAllocFn regalloc;
    AnvilSpillCostFn spill_cost;
    
    AnvilEmitPrologueFn emit_prologue;
    AnvilEmitEpilogueFn emit_epilogue;
    AnvilEmitInstFn emit_instruction;
    AnvilEmitLabelFn emit_label;
    AnvilEmitDataFn emit_data;
    
    AnvilPeepholeFn peephole_optimize;
    AnvilScheduleFn schedule_instructions;
    AnvilVectorizeFn vectorize;
    AnvilISelFn isel;
    
    AnvilRegNameFn reg_name_for_size;
    AnvilImmFitsFn immediate_fits;
    AnvilMaterializeConstFn materialize_constant;
    
    AnvilGetAbiFn get_abi;
} AnvilBackend;

#define ANVIL_MAX_BACKENDS 32

void anvil_backends_init(void);
void anvil_register_backend(int arch, AnvilBackend* backend);
AnvilBackend* anvil_get_backend(int arch);
const char* anvil_list_backends(void);

#ifdef __cplusplus
}
#endif

#endif
