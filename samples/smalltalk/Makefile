CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=

WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Werror
THREAD_FLAGS ?= -pthread
DEPFLAGS = -MMD -MP
INCLUDES = -Iinclude
BUILD_DIR ?= build
TOOLS_DIR = tools

HOST_PLATFORM ?= posix
ifeq ($(OS),Windows_NT)
HOST_PLATFORM = windows
endif
PLATFORM_OBJECT = $(BUILD_DIR)/platform_$(HOST_PLATFORM).o

LEXER_OBJECT = $(BUILD_DIR)/lexer.o
LEXER_TEST_OBJECT = $(BUILD_DIR)/lexer_test.o
LEXER_TEST = $(BUILD_DIR)/lexer_test
AST_OBJECT = $(BUILD_DIR)/ast.o
AST_TEST_OBJECT = $(BUILD_DIR)/ast_test.o
AST_TEST = $(BUILD_DIR)/ast_test
PARSER_OBJECT = $(BUILD_DIR)/parser.o
PARSER_TEST_OBJECT = $(BUILD_DIR)/parser_test.o
PARSER_TEST = $(BUILD_DIR)/parser_test
VALUE_OBJECT = $(BUILD_DIR)/value.o
VALUE_TEST_OBJECT = $(BUILD_DIR)/value_test.o
VALUE_TEST = $(BUILD_DIR)/value_test
SELECTOR_OBJECT = $(BUILD_DIR)/selector.o
SELECTOR_TEST_OBJECT = $(BUILD_DIR)/selector_test.o
SELECTOR_TEST = $(BUILD_DIR)/selector_test
SEMA_OBJECT = $(BUILD_DIR)/sema.o
SEMA_TEST_OBJECT = $(BUILD_DIR)/sema_test.o
SEMA_TEST = $(BUILD_DIR)/sema_test
SOURCE_BUNDLE_OBJECT = $(BUILD_DIR)/source_bundle.o
SOURCE_BUNDLE_TEST_OBJECT = $(BUILD_DIR)/source_bundle_test.o
SOURCE_BUNDLE_TEST = $(BUILD_DIR)/source_bundle_test
CLASS_GRAPH_OBJECT = $(BUILD_DIR)/class_graph.o
CLASS_GRAPH_TEST_OBJECT = $(BUILD_DIR)/class_graph_test.o
CLASS_GRAPH_TEST = $(BUILD_DIR)/class_graph_test
IMAGE_LAYOUT_OBJECT = $(BUILD_DIR)/image_layout.o
IMAGE_LAYOUT_TEST_OBJECT = $(BUILD_DIR)/image_layout_test.o
IMAGE_LAYOUT_TEST = $(BUILD_DIR)/image_layout_test
IMAGE_TEST_OBJECT = $(BUILD_DIR)/image_test.o
IMAGE_TEST = $(BUILD_DIR)/image_test
EXAMPLES_TEST_OBJECT = $(BUILD_DIR)/examples_test.o
EXAMPLES_TEST = $(BUILD_DIR)/examples_test
PRIMITIVE_OBJECT = $(BUILD_DIR)/primitive.o
PRIMITIVE_TEST_OBJECT = $(BUILD_DIR)/primitive_test.o
PRIMITIVE_TEST = $(BUILD_DIR)/primitive_test
RUNTIME_OBJECT = $(BUILD_DIR)/runtime.o $(PLATFORM_OBJECT)
RUNTIME_TEST_OBJECT = $(BUILD_DIR)/runtime_test.o
RUNTIME_TEST = $(BUILD_DIR)/runtime_test
CORE_PRIMITIVES_OBJECT = $(BUILD_DIR)/core_primitives.o
CORE_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/core_primitives_test.o
CORE_PRIMITIVES_TEST = $(BUILD_DIR)/core_primitives_test
LOOKUP_OBJECT = $(BUILD_DIR)/lookup.o
LOOKUP_TEST_OBJECT = $(BUILD_DIR)/lookup_test.o
LOOKUP_TEST = $(BUILD_DIR)/lookup_test
SEND_BRIDGE_OBJECT = $(BUILD_DIR)/send_bridge.o
SEND_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/send_bridge_test.o
SEND_BRIDGE_TEST = $(BUILD_DIR)/send_bridge_test
IMAGE_RUNTIME_OBJECT = $(BUILD_DIR)/image_runtime.o
IMAGE_RUNTIME_TEST_OBJECT = $(BUILD_DIR)/image_runtime_test.o
IMAGE_RUNTIME_TEST = $(BUILD_DIR)/image_runtime_test
PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/primitive_bridge.o
PRIMITIVE_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/primitive_bridge_test.o
PRIMITIVE_BRIDGE_TEST = $(BUILD_DIR)/primitive_bridge_test
HEAP_OBJECT = $(BUILD_DIR)/heap.o
HEAP_TEST_OBJECT = $(BUILD_DIR)/heap_test.o
HEAP_TEST = $(BUILD_DIR)/heap_test
HEAP_PRIMITIVES_OBJECT = $(BUILD_DIR)/heap_primitives.o
HEAP_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/heap_primitives_test.o
HEAP_PRIMITIVES_TEST = $(BUILD_DIR)/heap_primitives_test
FLOAT_PRIMITIVES_OBJECT = $(BUILD_DIR)/float_primitives.o
FLOAT_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/float_primitives_test.o
FLOAT_PRIMITIVES_TEST = $(BUILD_DIR)/float_primitives_test
FLOAT_PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/float_primitive_bridge.o
FLOAT_PRIMITIVE_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/float_primitive_bridge_test.o
FLOAT_PRIMITIVE_BRIDGE_TEST = $(BUILD_DIR)/float_primitive_bridge_test
INTEGER_PRIMITIVES_OBJECT = $(BUILD_DIR)/integer_primitives.o
INTEGER_PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/integer_primitive_bridge.o
INTEGER_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/integer_primitives_test.o
INTEGER_PRIMITIVES_TEST = $(BUILD_DIR)/integer_primitives_test
INTEGER_PRIMITIVE_BRIDGE_TEST_OBJECT = \
	$(BUILD_DIR)/integer_primitive_bridge_test.o
INTEGER_PRIMITIVE_BRIDGE_TEST = $(BUILD_DIR)/integer_primitive_bridge_test
STREAM_PRIMITIVES_OBJECT = $(BUILD_DIR)/stream_primitives.o
STREAM_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/stream_primitives_test.o
STREAM_PRIMITIVES_TEST = $(BUILD_DIR)/stream_primitives_test
STREAM_PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/stream_primitive_bridge.o
STREAM_PRIMITIVE_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/stream_primitive_bridge_test.o
STREAM_PRIMITIVE_BRIDGE_TEST = $(BUILD_DIR)/stream_primitive_bridge_test
STRING_PRIMITIVES_OBJECT = $(BUILD_DIR)/string_primitives.o
STRING_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/string_primitives_test.o
STRING_PRIMITIVES_TEST = $(BUILD_DIR)/string_primitives_test
STRING_PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/string_primitive_bridge.o
STRING_PRIMITIVE_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/string_primitive_bridge_test.o
STRING_PRIMITIVE_BRIDGE_TEST = $(BUILD_DIR)/string_primitive_bridge_test
SYMBOL_INTERN_OBJECT = $(BUILD_DIR)/symbol_intern.o
SYMBOL_INTERN_TEST_OBJECT = $(BUILD_DIR)/symbol_intern_test.o
SYMBOL_INTERN_TEST = $(BUILD_DIR)/symbol_intern_test
CONTROL_OBJECT = $(BUILD_DIR)/control.o
CONTROL_TEST_OBJECT = $(BUILD_DIR)/control_test.o
CONTROL_TEST = $(BUILD_DIR)/control_test
CONTROL_ROOTS_OBJECT = $(BUILD_DIR)/control_roots.o
CONTROL_BRIDGE_OBJECT = $(BUILD_DIR)/control_bridge.o
CONTROL_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/control_bridge_test.o
CONTROL_BRIDGE_TEST = $(BUILD_DIR)/control_bridge_test
CONTROL_GC_TEST_OBJECT = $(BUILD_DIR)/control_gc_test.o
CONTROL_GC_TEST = $(BUILD_DIR)/control_gc_test
CLOSURE_BRIDGE_OBJECT = $(BUILD_DIR)/closure_bridge.o
CLOSURE_BRIDGE_TEST_OBJECT = $(BUILD_DIR)/closure_bridge_test.o
CLOSURE_BRIDGE_TEST = $(BUILD_DIR)/closure_bridge_test
BLOCK_PRIMITIVES_OBJECT = $(BUILD_DIR)/block_primitives.o
BLOCK_PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/block_primitive_bridge.o
BLOCK_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/block_primitives_test.o
BLOCK_PRIMITIVES_TEST = $(BUILD_DIR)/block_primitives_test
EXCEPTION_PRIMITIVES_OBJECT = $(BUILD_DIR)/exception_primitives.o
EXCEPTION_PRIMITIVE_BRIDGE_OBJECT = $(BUILD_DIR)/exception_primitive_bridge.o
EXCEPTION_PRIMITIVES_TEST_OBJECT = $(BUILD_DIR)/exception_primitives_test.o
EXCEPTION_PRIMITIVES_TEST = $(BUILD_DIR)/exception_primitives_test
REFLECTION_PRIMITIVES_OBJECT = $(BUILD_DIR)/reflection_primitives.o
REFLECTION_PRIMITIVE_BRIDGE_OBJECT = \
	$(BUILD_DIR)/reflection_primitive_bridge.o
REFLECTION_PRIMITIVES_TEST_OBJECT = \
	$(BUILD_DIR)/reflection_primitives_test.o
REFLECTION_PRIMITIVES_TEST = $(BUILD_DIR)/reflection_primitives_test
AOT_BOOTSTRAP_OBJECT = $(BUILD_DIR)/aot_bootstrap.o
AOT_BOOTSTRAP_TEST_OBJECT = $(BUILD_DIR)/aot_bootstrap_test.o
AOT_BOOTSTRAP_TEST = $(BUILD_DIR)/aot_bootstrap_test
DNU_OBJECT = $(BUILD_DIR)/dnu.o
DNU_TEST_OBJECT = $(BUILD_DIR)/dnu_test.o
DNU_TEST = $(BUILD_DIR)/dnu_test
IMAGE_PRIMITIVE_CATALOG_TEST_OBJECT = \
	$(BUILD_DIR)/image_primitive_catalog_test.o
IMAGE_PRIMITIVE_CATALOG_TEST = $(BUILD_DIR)/image_primitive_catalog_test
TESTS = $(LEXER_TEST) $(AST_TEST) $(PARSER_TEST) $(VALUE_TEST) \
	$(SELECTOR_TEST) $(SEMA_TEST) $(SOURCE_BUNDLE_TEST) \
	$(CLASS_GRAPH_TEST) $(IMAGE_LAYOUT_TEST) $(IMAGE_TEST) $(EXAMPLES_TEST) \
	$(PRIMITIVE_TEST) $(RUNTIME_TEST) \
	$(CORE_PRIMITIVES_TEST) $(LOOKUP_TEST) $(SEND_BRIDGE_TEST) \
	$(IMAGE_RUNTIME_TEST) \
	$(PRIMITIVE_BRIDGE_TEST) $(HEAP_TEST) $(HEAP_PRIMITIVES_TEST) \
	$(FLOAT_PRIMITIVES_TEST) $(FLOAT_PRIMITIVE_BRIDGE_TEST) \
	$(INTEGER_PRIMITIVES_TEST) \
	$(INTEGER_PRIMITIVE_BRIDGE_TEST) $(STREAM_PRIMITIVES_TEST) \
	$(STREAM_PRIMITIVE_BRIDGE_TEST) \
	$(STRING_PRIMITIVES_TEST) $(STRING_PRIMITIVE_BRIDGE_TEST) \
	$(SYMBOL_INTERN_TEST) \
	$(CONTROL_TEST) $(CONTROL_BRIDGE_TEST) \
	$(CONTROL_GC_TEST) $(CLOSURE_BRIDGE_TEST) $(BLOCK_PRIMITIVES_TEST) \
	$(EXCEPTION_PRIMITIVES_TEST) $(REFLECTION_PRIMITIVES_TEST) \
	$(AOT_BOOTSTRAP_TEST) $(DNU_TEST) \
	$(IMAGE_PRIMITIVE_CATALOG_TEST)
ST_PARSE = $(BUILD_DIR)/st-parse
DEPS = $(LEXER_OBJECT:.o=.d) $(LEXER_TEST_OBJECT:.o=.d) \
	$(AST_OBJECT:.o=.d) $(AST_TEST_OBJECT:.o=.d) \
	$(PARSER_OBJECT:.o=.d) $(PARSER_TEST_OBJECT:.o=.d) \
	$(VALUE_OBJECT:.o=.d) $(VALUE_TEST_OBJECT:.o=.d) \
	$(SELECTOR_OBJECT:.o=.d) $(SELECTOR_TEST_OBJECT:.o=.d) \
	$(SEMA_OBJECT:.o=.d) $(SEMA_TEST_OBJECT:.o=.d) \
	$(SOURCE_BUNDLE_OBJECT:.o=.d) $(SOURCE_BUNDLE_TEST_OBJECT:.o=.d) \
	$(CLASS_GRAPH_OBJECT:.o=.d) $(CLASS_GRAPH_TEST_OBJECT:.o=.d) \
	$(IMAGE_LAYOUT_OBJECT:.o=.d) $(IMAGE_LAYOUT_TEST_OBJECT:.o=.d) \
	$(IMAGE_TEST_OBJECT:.o=.d) $(EXAMPLES_TEST_OBJECT:.o=.d) \
	$(PRIMITIVE_OBJECT:.o=.d) \
	$(PRIMITIVE_TEST_OBJECT:.o=.d) $(RUNTIME_OBJECT:.o=.d) \
	$(RUNTIME_TEST_OBJECT:.o=.d) $(CORE_PRIMITIVES_OBJECT:.o=.d) \
	$(CORE_PRIMITIVES_TEST_OBJECT:.o=.d) $(LOOKUP_OBJECT:.o=.d) \
	$(LOOKUP_TEST_OBJECT:.o=.d) $(SEND_BRIDGE_OBJECT:.o=.d) \
	$(SEND_BRIDGE_TEST_OBJECT:.o=.d) $(IMAGE_RUNTIME_OBJECT:.o=.d) \
	$(IMAGE_RUNTIME_TEST_OBJECT:.o=.d) $(PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(PRIMITIVE_BRIDGE_TEST_OBJECT:.o=.d) $(HEAP_OBJECT:.o=.d) \
	$(HEAP_TEST_OBJECT:.o=.d) $(HEAP_PRIMITIVES_OBJECT:.o=.d) \
	$(HEAP_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(FLOAT_PRIMITIVES_OBJECT:.o=.d) \
	$(FLOAT_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(FLOAT_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(FLOAT_PRIMITIVE_BRIDGE_TEST_OBJECT:.o=.d) \
	$(INTEGER_PRIMITIVES_OBJECT:.o=.d) \
	$(INTEGER_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(INTEGER_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(INTEGER_PRIMITIVE_BRIDGE_TEST_OBJECT:.o=.d) \
	$(STREAM_PRIMITIVES_OBJECT:.o=.d) \
	$(STREAM_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(STREAM_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(STREAM_PRIMITIVE_BRIDGE_TEST_OBJECT:.o=.d) \
	$(STRING_PRIMITIVES_OBJECT:.o=.d) \
	$(STRING_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(STRING_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(STRING_PRIMITIVE_BRIDGE_TEST_OBJECT:.o=.d) \
	$(SYMBOL_INTERN_OBJECT:.o=.d) $(SYMBOL_INTERN_TEST_OBJECT:.o=.d) \
	$(CONTROL_OBJECT:.o=.d) \
	$(CONTROL_TEST_OBJECT:.o=.d) $(CONTROL_ROOTS_OBJECT:.o=.d) \
	$(CONTROL_BRIDGE_OBJECT:.o=.d) \
	$(CONTROL_BRIDGE_TEST_OBJECT:.o=.d) $(CONTROL_GC_TEST_OBJECT:.o=.d) \
	$(CLOSURE_BRIDGE_OBJECT:.o=.d) $(CLOSURE_BRIDGE_TEST_OBJECT:.o=.d) \
	$(BLOCK_PRIMITIVES_OBJECT:.o=.d) \
	$(BLOCK_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(BLOCK_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(EXCEPTION_PRIMITIVES_OBJECT:.o=.d) \
	$(EXCEPTION_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(EXCEPTION_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(REFLECTION_PRIMITIVES_OBJECT:.o=.d) \
	$(REFLECTION_PRIMITIVE_BRIDGE_OBJECT:.o=.d) \
	$(REFLECTION_PRIMITIVES_TEST_OBJECT:.o=.d) \
	$(AOT_BOOTSTRAP_OBJECT:.o=.d) $(AOT_BOOTSTRAP_TEST_OBJECT:.o=.d) \
	$(DNU_OBJECT:.o=.d) $(DNU_TEST_OBJECT:.o=.d) \
	$(IMAGE_PRIMITIVE_CATALOG_TEST_OBJECT:.o=.d)

.PHONY: all test test-reference-corpus clean

all: $(TESTS) $(ST_PARSE)

test: $(TESTS)
	@for test in $(TESTS); do "$$test" || exit 1; done

test-reference-corpus: $(ST_PARSE)
	ST_PARSE_BIN=$(abspath $(ST_PARSE)) bash tests/run_reference_corpus.sh

$(LEXER_TEST): $(LEXER_OBJECT) $(LEXER_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/lexer.o: src/frontend/lexer.c include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/lexer_test.o: tests/lexer_test.c include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(AST_TEST): $(AST_OBJECT) $(AST_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/ast.o: src/frontend/ast.c include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/ast_test.o: tests/ast_test.c include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(PARSER_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) $(PARSER_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/parser.o: src/frontend/parser.c include/st_parser.h include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/parser_test.o: tests/parser_test.c include/st_parser.h include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(VALUE_TEST): $(VALUE_OBJECT) $(VALUE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/value.o: src/runtime/value.c include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/value_test.o: tests/value_test.c include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(SELECTOR_TEST): $(SELECTOR_OBJECT) $(SELECTOR_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/selector.o: src/frontend/selector.c include/st_selector.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/selector_test.o: tests/selector_test.c include/st_selector.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(SEMA_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) $(SEMA_OBJECT) $(SEMA_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/sema.o: src/frontend/sema.c include/st_sema.h include/st_ast.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/sema_test.o: tests/sema_test.c include/st_sema.h include/st_parser.h include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(SOURCE_BUNDLE_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(SOURCE_BUNDLE_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/source_bundle.o: src/frontend/source_bundle.c include/st_source_bundle.h \
		include/st_parser.h include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/source_bundle_test.o: tests/source_bundle_test.c \
		include/st_source_bundle.h include/st_parser.h include/st_ast.h \
		include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(CLASS_GRAPH_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SEMA_OBJECT) $(CLASS_GRAPH_OBJECT) $(CLASS_GRAPH_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/class_graph.o: src/frontend/class_graph.c include/st_class_graph.h \
		include/st_sema.h include/st_ast.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/class_graph_test.o: tests/class_graph_test.c \
		include/st_class_graph.h include/st_sema.h include/st_parser.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(IMAGE_LAYOUT_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SEMA_OBJECT) $(CLASS_GRAPH_OBJECT) $(IMAGE_LAYOUT_OBJECT) \
		$(IMAGE_LAYOUT_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/image_layout.o: src/compiler/image_layout.c \
		include/st_image_layout.h include/st_class_graph.h include/st_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) -I../../include \
		$(INCLUDES) -c $< -o $@

$(BUILD_DIR)/image_layout_test.o: tests/image_layout_test.c \
		include/st_image_layout.h include/st_parser.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) -I../../include \
		$(INCLUDES) -c $< -o $@

$(IMAGE_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) $(SEMA_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(CLASS_GRAPH_OBJECT) $(IMAGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/image_test.o: tests/image_test.c include/st_source_bundle.h \
		include/st_class_graph.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(EXAMPLES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) $(SEMA_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(CLASS_GRAPH_OBJECT) $(EXAMPLES_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/examples_test.o: tests/examples_test.c \
		include/st_source_bundle.h include/st_class_graph.h include/st_sema.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(PRIMITIVE_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(PRIMITIVE_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/primitive.o: src/compiler/primitive.c include/st_primitive.h include/st_ast.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/primitive_test.o: tests/primitive_test.c include/st_primitive.h \
		include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(RUNTIME_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(RUNTIME_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/runtime.o: src/runtime/runtime.c include/st_runtime.h \
		include/st_dispatch.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/runtime_test.o: tests/runtime_test.c include/st_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(CORE_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(VALUE_OBJECT) $(PRIMITIVE_OBJECT) \
		$(CORE_PRIMITIVES_OBJECT) $(CORE_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/core_primitives.o: src/runtime/primitives/core_primitives.c \
		include/st_core_primitives.h include/st_primitive.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/core_primitives_test.o: tests/core_primitives_test.c \
		include/st_core_primitives.h include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(LOOKUP_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(LOOKUP_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/lookup.o: src/runtime/lookup.c include/st_lookup.h \
		include/st_runtime.h include/st_selector.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/lookup_test.o: tests/lookup_test.c include/st_lookup.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(SEND_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(SEND_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/send_bridge.o: src/runtime/send_bridge.c include/st_send_bridge.h \
		include/st_lookup.h include/st_runtime.h include/st_value.h \
		include/st_control.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/send_bridge_test.o: tests/send_bridge_test.c \
		include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(IMAGE_RUNTIME_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(IMAGE_RUNTIME_OBJECT) $(IMAGE_RUNTIME_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/image_runtime.o: src/runtime/image_runtime.c \
		include/st_image_runtime.h include/st_send_bridge.h include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/image_runtime_test.o: tests/image_runtime_test.c \
		include/st_image_runtime.h include/st_send_bridge.h include/st_lookup.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(PRIMITIVE_BRIDGE_TEST): $(PRIMITIVE_BRIDGE_OBJECT) \
		$(PRIMITIVE_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/primitive_bridge.o: src/runtime/primitives/primitive_bridge.c \
		include/st_primitive_bridge.h include/st_core_primitives.h \
		include/st_dispatch.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/primitive_bridge_test.o: tests/primitive_bridge_test.c \
		include/st_primitive_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(HEAP_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) \
		$(HEAP_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/heap.o: src/runtime/heap.c include/st_heap.h include/st_runtime.h \
		include/st_value.h include/st_dispatch.h include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/heap_test.o: tests/heap_test.c include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(HEAP_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(CORE_PRIMITIVES_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) $(HEAP_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/heap_primitives.o: src/runtime/primitives/heap_primitives.c \
		include/st_heap_primitives.h include/st_heap.h \
		include/st_core_primitives.h include/st_primitive.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/heap_primitives_test.o: tests/heap_primitives_test.c \
		include/st_heap_primitives.h include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(FLOAT_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(CORE_PRIMITIVES_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) $(FLOAT_PRIMITIVES_OBJECT) \
		$(FLOAT_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/float_primitives.o: src/runtime/primitives/float_primitives.c \
		src/runtime/primitives/float_primitives_internal.h \
		include/st_float_primitives.h include/st_heap.h \
		include/st_primitive.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/float_primitives_test.o: tests/float_primitives_test.c \
		src/runtime/primitives/float_primitives_internal.h \
		include/st_float_primitives.h include/st_core_primitives.h \
		include/st_heap_primitives.h include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(FLOAT_PRIMITIVE_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) $(FLOAT_PRIMITIVES_OBJECT) \
		$(INTEGER_PRIMITIVES_OBJECT) $(FLOAT_PRIMITIVE_BRIDGE_OBJECT) \
		$(FLOAT_PRIMITIVE_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/float_primitive_bridge.o: \
		src/runtime/primitives/float_primitive_bridge.c \
		src/runtime/primitives/float_primitives_internal.h \
		include/st_float_primitive_bridge.h include/st_float_primitives.h \
		include/st_integer_primitives.h include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/float_primitive_bridge_test.o: \
		tests/float_primitive_bridge_test.c \
		include/st_float_primitive_bridge.h include/st_float_primitives.h \
		include/st_integer_primitives.h include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(INTEGER_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) \
		$(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) \
		$(FLOAT_PRIMITIVES_OBJECT) $(INTEGER_PRIMITIVES_OBJECT) \
		$(INTEGER_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/integer_primitives.o: \
		src/runtime/primitives/integer_primitives.c \
		include/st_integer_primitives.h include/st_float_primitives.h \
		include/st_heap.h include/st_primitive.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/integer_primitives_test.o: tests/integer_primitives_test.c \
		include/st_integer_primitives.h include/st_float_primitives.h \
		include/st_heap.h include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(INTEGER_PRIMITIVE_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) $(FLOAT_PRIMITIVES_OBJECT) \
		$(INTEGER_PRIMITIVES_OBJECT) $(INTEGER_PRIMITIVE_BRIDGE_OBJECT) \
		$(INTEGER_PRIMITIVE_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/integer_primitive_bridge.o: \
		src/runtime/primitives/integer_primitive_bridge.c \
		include/st_integer_primitives.h include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/integer_primitive_bridge_test.o: \
		tests/integer_primitive_bridge_test.c \
		include/st_integer_primitives.h include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(STREAM_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(CORE_PRIMITIVES_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) $(FLOAT_PRIMITIVES_OBJECT) \
		$(STREAM_PRIMITIVES_OBJECT) $(STREAM_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/stream_primitives.o: src/runtime/primitives/stream_primitives.c \
		include/st_stream_primitives.h include/st_heap.h \
		include/st_primitive.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/stream_primitives_test.o: tests/stream_primitives_test.c \
		include/st_stream_primitives.h include/st_core_primitives.h \
		include/st_heap_primitives.h include/st_float_primitives.h \
		include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(STREAM_PRIMITIVE_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) $(PRIMITIVE_OBJECT) \
		$(STREAM_PRIMITIVES_OBJECT) $(STREAM_PRIMITIVE_BRIDGE_OBJECT) \
		$(STREAM_PRIMITIVE_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/stream_primitive_bridge.o: src/runtime/primitives/stream_primitive_bridge.c \
		include/st_stream_primitive_bridge.h include/st_stream_primitives.h \
		include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/stream_primitive_bridge_test.o: \
		tests/stream_primitive_bridge_test.c \
		include/st_stream_primitive_bridge.h include/st_stream_primitives.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(STRING_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(CORE_PRIMITIVES_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) $(FLOAT_PRIMITIVES_OBJECT) \
		$(STREAM_PRIMITIVES_OBJECT) $(STRING_PRIMITIVES_OBJECT) \
		$(STRING_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/string_primitives.o: src/runtime/primitives/string_primitives.c \
		include/st_string_primitives.h include/st_heap.h \
		include/st_primitive.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/string_primitives_test.o: tests/string_primitives_test.c \
		include/st_string_primitives.h include/st_stream_primitives.h \
		include/st_core_primitives.h include/st_heap_primitives.h \
		include/st_float_primitives.h include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(STRING_PRIMITIVE_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) $(STRING_PRIMITIVES_OBJECT) \
		$(STRING_PRIMITIVE_BRIDGE_OBJECT) \
		$(STRING_PRIMITIVE_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/string_primitive_bridge.o: \
		src/runtime/primitives/string_primitive_bridge.c \
		include/st_string_primitive_bridge.h include/st_string_primitives.h \
		include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/string_primitive_bridge_test.o: \
		tests/string_primitive_bridge_test.c \
		include/st_string_primitive_bridge.h include/st_string_primitives.h \
		include/st_send_bridge.h include/st_lookup.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(SYMBOL_INTERN_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) \
		$(STRING_PRIMITIVES_OBJECT) \
		$(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(IMAGE_RUNTIME_OBJECT) $(SYMBOL_INTERN_OBJECT) \
		$(SYMBOL_INTERN_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/symbol_intern.o: src/runtime/primitives/symbol_intern.c \
		include/st_symbol_intern.h include/st_image_runtime.h \
		include/st_send_bridge.h include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/symbol_intern_test.o: tests/symbol_intern_test.c \
		include/st_symbol_intern.h include/st_image_runtime.h \
		include/st_send_bridge.h include/st_lookup.h \
		include/st_source_bundle.h include/st_string_primitives.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(CONTROL_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/control.o: src/runtime/control/control.c include/st_control.h \
		include/st_runtime.h include/st_dispatch.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/control_test.o: tests/control_test.c include/st_control.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/control_roots.o: src/runtime/control/control_roots.c \
		include/st_control_roots.h include/st_control.h \
		include/st_send_bridge.h include/st_runtime.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(CONTROL_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) $(CONTROL_BRIDGE_OBJECT) \
		$(CONTROL_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/control_bridge.o: src/runtime/control/control_bridge.c \
		include/st_control_bridge.h include/st_send_bridge.h \
		include/st_control.h include/st_runtime.h include/st_value.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/control_bridge_test.o: tests/control_bridge_test.c \
		include/st_control_bridge.h include/st_send_bridge.h \
		include/st_control.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(CONTROL_GC_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(CONTROL_GC_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/control_gc_test.o: tests/control_gc_test.c \
		include/st_heap.h include/st_control_roots.h include/st_control.h \
		include/st_send_bridge.h include/st_lookup.h include/st_runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(CLOSURE_BRIDGE_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(CONTROL_BRIDGE_OBJECT) $(HEAP_OBJECT) $(CLOSURE_BRIDGE_OBJECT) \
		$(CLOSURE_BRIDGE_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/closure_bridge.o: src/runtime/closure_bridge.c \
		include/st_closure_bridge.h include/st_send_bridge.h \
		include/st_heap.h include/st_control.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/closure_bridge_test.o: tests/closure_bridge_test.c \
		include/st_closure_bridge.h include/st_control_bridge.h \
		include/st_control_roots.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BLOCK_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) \
		$(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) $(CONTROL_BRIDGE_OBJECT) \
		$(HEAP_OBJECT) $(CLOSURE_BRIDGE_OBJECT) $(BLOCK_PRIMITIVES_OBJECT) \
		$(BLOCK_PRIMITIVE_BRIDGE_OBJECT) $(BLOCK_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/block_primitives.o: \
		src/runtime/primitives/block_primitives.c \
		src/runtime/primitives/block_primitives_internal.h \
		include/st_block_primitives.h include/st_closure_bridge.h \
		include/st_primitive.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/block_primitive_bridge.o: \
		src/runtime/primitives/block_primitive_bridge.c \
		src/runtime/primitives/block_primitives_internal.h \
		include/st_block_primitive_bridge.h include/st_block_primitives.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/block_primitives_test.o: tests/block_primitives_test.c \
		include/st_block_primitive_bridge.h include/st_control_bridge.h \
		include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(EXCEPTION_PRIMITIVES_TEST): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) \
		$(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) $(CONTROL_BRIDGE_OBJECT) \
		$(HEAP_OBJECT) $(CLOSURE_BRIDGE_OBJECT) \
		$(EXCEPTION_PRIMITIVES_OBJECT) $(EXCEPTION_PRIMITIVE_BRIDGE_OBJECT) \
		$(EXCEPTION_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/exception_primitives.o: \
		src/runtime/primitives/exception_primitives.c \
		src/runtime/primitives/exception_primitives_internal.h \
		include/st_exception_primitives.h include/st_closure_bridge.h \
		include/st_control.h include/st_primitive.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/exception_primitive_bridge.o: \
		src/runtime/primitives/exception_primitive_bridge.c \
		src/runtime/primitives/exception_primitives_internal.h \
		include/st_exception_primitive_bridge.h \
		include/st_exception_primitives.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/exception_primitives_test.o: tests/exception_primitives_test.c \
		include/st_exception_primitive_bridge.h include/st_control_bridge.h \
		include/st_source_bundle.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(REFLECTION_PRIMITIVES_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) $(IMAGE_RUNTIME_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) \
		$(REFLECTION_PRIMITIVES_OBJECT) \
		$(REFLECTION_PRIMITIVE_BRIDGE_OBJECT) \
		$(REFLECTION_PRIMITIVES_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/reflection_primitives.o: \
		src/runtime/primitives/reflection_primitives.c \
		include/st_reflection_primitives.h include/st_image_runtime.h \
		include/st_lookup.h include/st_primitive.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/reflection_primitive_bridge.o: \
		src/runtime/primitives/reflection_primitive_bridge.c \
		include/st_reflection_primitive_bridge.h \
		include/st_reflection_primitives.h include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/reflection_primitives_test.o: \
		tests/reflection_primitives_test.c \
		include/st_heap_primitives.h \
		include/st_reflection_primitive_bridge.h \
		include/st_reflection_primitives.h include/st_send_bridge.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(AOT_BOOTSTRAP_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) \
		$(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) \
		$(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) $(IMAGE_RUNTIME_OBJECT) \
		$(CORE_PRIMITIVES_OBJECT) $(HEAP_PRIMITIVES_OBJECT) \
		$(SYMBOL_INTERN_OBJECT) $(REFLECTION_PRIMITIVES_OBJECT) \
		$(AOT_BOOTSTRAP_OBJECT) $(AOT_BOOTSTRAP_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/aot_bootstrap.o: src/runtime/aot_bootstrap.c \
		include/st_aot_bootstrap.h include/st_image_emit.h \
		include/st_reflection_primitives.h include/st_symbol_intern.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/aot_bootstrap_test.o: tests/aot_bootstrap_test.c \
		include/st_aot_bootstrap.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(DNU_TEST): $(VALUE_OBJECT) $(RUNTIME_OBJECT) $(LOOKUP_OBJECT) \
		$(SEND_BRIDGE_OBJECT) $(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) \
		$(HEAP_OBJECT) $(IMAGE_RUNTIME_OBJECT) $(CORE_PRIMITIVES_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) $(SYMBOL_INTERN_OBJECT) \
		$(REFLECTION_PRIMITIVES_OBJECT) $(AOT_BOOTSTRAP_OBJECT) \
		$(DNU_OBJECT) $(DNU_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@

$(BUILD_DIR)/dnu.o: src/runtime/dnu.c include/st_dnu.h \
		include/st_aot_bootstrap.h include/st_send_bridge.h \
		include/st_heap.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/dnu_test.o: tests/dnu_test.c include/st_dnu.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(IMAGE_PRIMITIVE_CATALOG_TEST): \
		$(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) \
		$(SOURCE_BUNDLE_OBJECT) $(PRIMITIVE_OBJECT) $(VALUE_OBJECT) \
		$(RUNTIME_OBJECT) $(LOOKUP_OBJECT) $(SEND_BRIDGE_OBJECT) \
		$(CONTROL_OBJECT) $(CONTROL_ROOTS_OBJECT) $(HEAP_OBJECT) \
		$(IMAGE_RUNTIME_OBJECT) \
		$(CLOSURE_BRIDGE_OBJECT) $(CORE_PRIMITIVES_OBJECT) \
		$(HEAP_PRIMITIVES_OBJECT) $(FLOAT_PRIMITIVES_OBJECT) \
		$(INTEGER_PRIMITIVES_OBJECT) $(STREAM_PRIMITIVES_OBJECT) \
		$(STRING_PRIMITIVES_OBJECT) $(BLOCK_PRIMITIVES_OBJECT) \
		$(EXCEPTION_PRIMITIVES_OBJECT) $(REFLECTION_PRIMITIVES_OBJECT) \
		$(IMAGE_PRIMITIVE_CATALOG_TEST_OBJECT)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $^ -o $@ -lm

$(BUILD_DIR)/image_primitive_catalog_test.o: \
		tests/image_primitive_catalog_test.c \
		include/st_source_bundle.h include/st_primitive.h \
		include/st_core_primitives.h include/st_heap_primitives.h \
		include/st_float_primitives.h include/st_integer_primitives.h \
		include/st_stream_primitives.h include/st_string_primitives.h \
		include/st_block_primitives.h include/st_exception_primitives.h \
		include/st_reflection_primitives.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) \
		-I../../include $(INCLUDES) -c $< -o $@

$(ST_PARSE): $(LEXER_OBJECT) $(AST_OBJECT) $(PARSER_OBJECT) $(BUILD_DIR)/st_parse.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/st_parse.o: $(TOOLS_DIR)/st_parse.c include/st_parser.h include/st_ast.h include/st_lexer.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

$(PLATFORM_OBJECT): src/platform/$(HOST_PLATFORM)/runtime.c src/platform/runtime.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@
