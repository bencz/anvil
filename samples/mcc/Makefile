# MCC - Micro C Compiler
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -I../../include -Iinclude
LDFLAGS = -L../../lib -lanvil

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
BINDIR = .

# Source files
SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Target
TARGET = $(BINDIR)/mcc

# Default target
all: $(TARGET)

# Create directories
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Link
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Compile
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Dependencies
$(OBJDIR)/main.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h
$(OBJDIR)/lexer.o: $(INCDIR)/lexer.h $(INCDIR)/token.h $(INCDIR)/c_std.h
$(OBJDIR)/preprocessor.o: $(INCDIR)/preprocessor.h $(INCDIR)/lexer.h $(INCDIR)/c_std.h
$(OBJDIR)/parser.o: $(INCDIR)/parser.h $(INCDIR)/ast.h $(INCDIR)/lexer.h $(INCDIR)/c_std.h
$(OBJDIR)/ast.o: $(INCDIR)/ast.h
$(OBJDIR)/types.o: $(INCDIR)/types.h
$(OBJDIR)/symtab.o: $(INCDIR)/symtab.h $(INCDIR)/types.h
$(OBJDIR)/sema.o: $(INCDIR)/sema.h $(INCDIR)/ast.h $(INCDIR)/symtab.h $(INCDIR)/c_std.h
$(OBJDIR)/codegen.o: $(INCDIR)/codegen.h $(INCDIR)/ast.h
$(OBJDIR)/c_std.o: $(INCDIR)/c_std.h $(INCDIR)/mcc.h
$(OBJDIR)/context.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h

# Clean
clean:
	rm -rf $(OBJDIR) $(TARGET)

# Run tests
test: $(TARGET)
	@echo "Running tests..."
	@for f in tests/*.c; do \
		echo "Testing $$f..."; \
		./$(TARGET) -fsyntax-only $$f && echo "  OK" || echo "  FAILED"; \
	done

# Install (to parent project)
install: $(TARGET)
	cp $(TARGET) ../../bin/

.PHONY: all clean test install
