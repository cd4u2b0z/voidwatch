CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS  += -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP
LDFLAGS ?=
LDLIBS  ?= -lm -lasound -lfftw3f -lpthread

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := voidwatch

# Install paths. Default $(PREFIX)=$HOME/.local — user-local, no doas
# needed, $HOME/.local/bin is already on $PATH for arch + omz setups.
# Override at install time:  make install PREFIX=/usr/local
PREFIX ?= $(HOME)/.local
BINDIR  = $(PREFIX)/bin

.DEFAULT_GOAL := all

-include $(DEP)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

astro: $(BIN)
	./$(BIN) --astro

clean:
	rm -f $(OBJ) $(DEP) $(BIN) $(TEST_BINS) $(TEST_DEP)

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	@echo "installed: $(BINDIR)/$(BIN)"

uninstall:
	rm -f $(BINDIR)/$(BIN)
	@echo "uninstalled: $(BINDIR)/$(BIN)"

# ---- tests ----------------------------------------------------------------
# Plain C asserts in tests/. Each binary gets the minimum object set it needs
# (no audio / palette / astro pulled in unless the test specifically wants
# them) so a test can break a module without taking the whole suite with it.

TEST_SRC  := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRC:.c=)
TEST_DEP  := $(TEST_SRC:.c=.d)

-include $(TEST_DEP)

# Per-binary object dependencies. Add new tests by extending this block.
tests/test_framebuffer: tests/test_framebuffer.c src/framebuffer.o
	$(CC) $(CFLAGS) $^ -o $@ -lm

tests/test_projection: tests/test_projection.c src/ephem.o
	$(CC) $(CFLAGS) $^ -o $@ -lm

tests/test_nbody: tests/test_nbody.c src/body.o src/vwconfig.o src/palette.o
	$(CC) $(CFLAGS) $^ -o $@ -lm

tests/test_json: tests/test_json.c
	$(CC) $(CFLAGS) $^ -o $@

test: $(TEST_BINS) $(BIN)
	@set -e; \
	pass=0; fail=0; \
	for t in $(TEST_BINS); do \
	    echo "==> $$t"; \
	    if ./$$t; then pass=$$((pass+1)); \
	    else fail=$$((fail+1)); fi; \
	done; \
	echo; \
	echo "tests: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

.PHONY: all run astro clean install uninstall test
