# Abyss compiler build
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := abyssc

.PHONY: all clean run native

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) examples/run_demo.aby

# transpile an example to C, compile it natively, and run the binary
native: $(BIN)
	@mkdir -p build
	./$(BIN) --emit-c examples/run_demo.aby > build/run_demo.c
	$(CC) -O2 build/run_demo.c -o build/run_demo
	./build/run_demo

clean:
	rm -f $(OBJ) $(BIN)
