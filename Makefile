CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS  += -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP
LDFLAGS ?=
LDLIBS  ?= -lm -lasound -lfftw3f -lpthread

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)
BIN := voidwatch

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

.PHONY: all run astro clean
