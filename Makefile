# odin - just run `make`. needs a c compiler and zlib (almost every system has it)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Isrc
LDLIBS  += -lz -lm
PREFIX  ?= /usr/local

SRC  := $(shell find src -name '*.c')
OBJ  := $(SRC:src/%.c=build/%.o)
LIB  := $(filter-out build/main.o,$(OBJ))

odin: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# unit tests, built with address sanitizer so memory bugs blow up loudly
test: $(SRC)
	@mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -Isrc -g -fsanitize=address,undefined -o build/test_odin \
		tests/test_meta.c $(filter-out src/main.c,$(SRC)) $(LDLIBS)
	./build/test_odin

# throws random junk at the parsers (needs clang). FUZZ_TIME is in seconds
FUZZ_TIME ?= 60
fuzz: $(SRC)
	@mkdir -p build/corpus
	clang -std=gnu11 -Isrc -g -O1 -fsanitize=fuzzer,address,undefined -o build/fuzz_odin \
		tests/fuzz_parse.c $(filter-out src/main.c,$(SRC)) $(LDLIBS)
	./build/fuzz_odin -max_total_time=$(FUZZ_TIME) -max_len=65536 build/corpus

install: odin
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 odin $(DESTDIR)$(PREFIX)/bin/odin

clean:
	rm -rf build odin

.PHONY: test fuzz install clean
-include $(OBJ:.o=.d)
