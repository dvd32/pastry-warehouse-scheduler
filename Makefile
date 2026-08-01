CC = gcc
CFLAGS += -Wall -Werror -std=gnu11 -O2
LDFLAGS += -lm
SRC = src/api.c
BIN = pasticceria

.PHONY: all clean debug

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

debug: CFLAGS += -g3 -fsanitize=address
debug: $(BIN)

clean:
	rm -f $(BIN)
