# Abyss compiler build
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := abyssc

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) examples/demo.aby

clean:
	rm -f $(OBJ) $(BIN)
