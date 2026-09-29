# gbemu - a Game Boy emulator in C

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Isrc
CFLAGS  += $(shell pkg-config --cflags sdl2)
LDLIBS  += $(shell pkg-config --libs sdl2)

SRCS = src/main.c src/cpu.c src/mmu.c src/gpu.c src/apu.c \
       src/timer.c src/joypad.c src/cart.c
OBJS = $(SRCS:.c=.o)
BIN  = gbemu

.PHONY: all clean testrom run

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c src/gb.h src/cart.h src/apu.h src/joypad.h src/types.h
	$(CC) $(CFLAGS) -c -o $@ $<

testrom:
	python3 tools/gen_test_rom.py roms/test.gb

run: $(BIN)
	./$(BIN) roms/test.gb

clean:
	rm -f $(OBJS) $(BIN)
