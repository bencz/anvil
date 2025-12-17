# MCC - Micro C Compiler
# Build Rules
#
# This file contains compilation rules and dependencies.

# Create directories
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Link
$(TARGET): $(ALL_OBJS)
	$(CC) $(ALL_OBJS) -o $@ $(LDFLAGS)

# Compile main source files
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile preprocessor source files
$(OBJDIR)/pp_%.o: $(PP_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile lexer source files
$(OBJDIR)/lex_%.o: $(LEX_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile parser source files
$(OBJDIR)/parse_%.o: $(PARSE_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile sema source files
$(OBJDIR)/sema_%.o: $(SEMA_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile codegen source files
$(OBJDIR)/cg_%.o: $(CODEGEN_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile optimization source files
$(OBJDIR)/opt_%.o: $(OPT_SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ============================================================
# Dependencies - Main source files
# ============================================================
$(OBJDIR)/main.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h
$(OBJDIR)/lexer.o: $(INCDIR)/lexer.h $(INCDIR)/token.h $(INCDIR)/c_std.h
$(OBJDIR)/parser.o: $(INCDIR)/parser.h $(INCDIR)/ast.h $(INCDIR)/lexer.h $(INCDIR)/c_std.h
$(OBJDIR)/ast.o: $(INCDIR)/ast.h
$(OBJDIR)/types.o: $(INCDIR)/types.h
$(OBJDIR)/symtab.o: $(INCDIR)/symtab.h $(INCDIR)/types.h
$(OBJDIR)/codegen.o: $(INCDIR)/codegen.h $(INCDIR)/ast.h
$(OBJDIR)/c_std.o: $(INCDIR)/c_std.h $(INCDIR)/mcc.h
$(OBJDIR)/context.o: $(INCDIR)/mcc.h $(INCDIR)/c_std.h

# ============================================================
# Dependencies - Preprocessor source files
# ============================================================
$(OBJDIR)/pp_preprocessor.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_macro.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_expr.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_directive.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/pp_include.o: $(PP_INTERNAL_H) $(INCDIR)/mcc.h

# ============================================================
# Dependencies - Lexer source files
# ============================================================
$(OBJDIR)/lex_lexer.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_token.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_char.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_comment.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_string.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_number.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_ident.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/lex_operator.o: $(LEX_INTERNAL_H) $(INCDIR)/mcc.h

# ============================================================
# Dependencies - Parser source files
# ============================================================
$(OBJDIR)/parse_parser.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_expr.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_stmt.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_type.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/parse_decl.o: $(PARSE_INTERNAL_H) $(INCDIR)/mcc.h

# ============================================================
# Dependencies - Sema source files
# ============================================================
$(OBJDIR)/sema_sema.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_expr.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_stmt.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_decl.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_type.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/sema_const.o: $(SEMA_INTERNAL_H) $(INCDIR)/mcc.h

# ============================================================
# Dependencies - Codegen source files
# ============================================================
$(OBJDIR)/cg_codegen.o: $(CODEGEN_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/cg_codegen_expr.o: $(CODEGEN_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/cg_codegen_stmt.o: $(CODEGEN_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/cg_codegen_decl.o: $(CODEGEN_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/cg_codegen_type.o: $(CODEGEN_INTERNAL_H) $(INCDIR)/mcc.h

# ============================================================
# Dependencies - Optimization source files
# ============================================================
$(OBJDIR)/opt_ast_opt.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h $(INCDIR)/ast_opt.h
$(OBJDIR)/opt_opt_helpers.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_const.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_simplify.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_dead.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_propagate.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_cse.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_loop.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_inline.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
$(OBJDIR)/opt_opt_stubs.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
