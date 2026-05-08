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
	rm -f $(OBJ) $(DEP) $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	@echo "installed: $(BINDIR)/$(BIN)"

uninstall:
	rm -f $(BINDIR)/$(BIN)
	@echo "uninstalled: $(BINDIR)/$(BIN)"

.PHONY: all run astro clean install uninstall
