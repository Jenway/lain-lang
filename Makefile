# Lain Compiler — Makefile
#
# Targets:
#   make              — debug build of lainc
#   make release      — release build of lainc
#   make l1c          — L1 IR emitter tool
#   make l1i          — L1 IR interpreter tool
#   make test         — run full test suite
#   make clean        — remove build artifacts

CC       := gcc
CFLAGS   := -Isrc -I third_party/chibi-scheme/include -std=gnu11
LDFLAGS  := -L third_party/chibi-scheme -lchibi-scheme -lm -ldl \
            -Wl,-rpath,$(CURDIR)/third_party/chibi-scheme

BUILD    := build
DEBUG    := $(BUILD)/debug
RELEASE  := $(BUILD)/release

# ── Main compiler sources ──────────────────────────────────────────────

LAINC_SRCS := \
    src/compiler/native_runtime.c \
    src/compiler/vm_chibi.c       \
    src/compiler/lainir_exec.c    \
    src/compiler/native_compiler.c \
    src/compiler/builder_ffi.c    \
    src/lainir/lainir_core.c      \
    src/lainast/lain_ast.c        \
    src/lainast/lain_ast_parser.c \
    src/lainir/emitter.c          \
    src/lainir/emit_text.c        \
    src/lainir/interpreter.c

LAINC_OBJS := $(LAINC_SRCS:%.c=$(DEBUG)/%.o)

# ── L1 tool sources ────────────────────────────────────────────────────

L1_SRCS := \
    src/lainir/lain_ir_parser.c   \
    src/lainir/lainir_core.c

# ── Default target ─────────────────────────────────────────────────────

.PHONY: all
all: lainc

# ── Debug build ────────────────────────────────────────────────────────

lainc: CFLAGS += -Wall -Wextra -g -O0
lainc: $(LAINC_OBJS)
	@mkdir -p $(dir src/compiler)
	$(CC) $^ $(LDFLAGS) -o src/compiler/lainc

$(DEBUG)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── Release build ──────────────────────────────────────────────────────

.PHONY: release
release: CFLAGS += -O2 -DNDEBUG
release: LAINC_OBJS := $(LAINC_SRCS:%.c=$(RELEASE)/%.o)
release: $(LAINC_OBJS)
	@mkdir -p $(dir $(RELEASE))
	$(CC) $^ $(LDFLAGS) -o src/compiler/lainc

$(RELEASE)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ── L1 tools ───────────────────────────────────────────────────────────

.PHONY: l1c l1i
l1c: CFLAGS += -g -O0
l1c: $(L1_SRCS:%.c=$(DEBUG)/%.o) $(DEBUG)/src/lainir/emitter.o $(DEBUG)/src/lainir/emit_text.o $(DEBUG)/src/lainir/lain_ir_main.o
	$(CC) $^ $(LDFLAGS) -o $(BUILD)/l1c

l1i: CFLAGS += -g -O0
l1i: $(L1_SRCS:%.c=$(DEBUG)/%.o) $(DEBUG)/src/lainir/interpreter.o $(DEBUG)/src/lainir/lain_ir_interp_main.o
	$(CC) $^ $(LDFLAGS) -o $(BUILD)/l1i

# ── Test ───────────────────────────────────────────────────────────────

.PHONY: test
test: lainc
	python3 tests/runner.py; true

# ── Clean ──────────────────────────────────────────────────────────────

.PHONY: clean
clean:
	rm -rf $(BUILD) src/compiler/lainc src/lainir/l1c src/lainir/l1i
