# odin - just run `make`. needs a c compiler, zlib and libcurl (almost every system has them)
#
#   make              normal build
#   make STATIC=1     bake zlib + libcurl into the binary (what the release downloads use)
#   make test         unit tests + username engine vs fake sites
#   make install      copy it to /usr/local/bin

CC       ?= cc
# CFLAGS is yours to override (make CFLAGS="-O3 -march=native"), odin's must-haves live in
# ODIN_CFLAGS so a command-line CFLAGS can't wipe out -Isrc and friends
CFLAGS   ?= -O2 -g
ODIN_CFLAGS := -std=gnu11 -Wall -Wextra -Isrc
DEPFLAGS ?= -MMD -MP
LDLIBS   += -lz -lm -lcurl
PREFIX   ?= /usr/local
SAN      ?= -fsanitize=address,undefined

ifeq ($(OS),Windows_NT)
  EXE     := odin.exe
  # mingw's c99 printf (%zu and friends)
  ODIN_CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
  RES     := build/odin_res.o
  # no sanitizers in mingw
  SAN     :=
else
  EXE     := odin
  RES     :=
endif

ifeq ($(STATIC),1)
  ODIN_CFLAGS += -DCURL_STATICLIB
  LDLIBS  := $(shell pkg-config --static --libs libcurl zlib) -lm
  LDFLAGS += -static
endif

SRC  := $(shell find src -name '*.c')
OBJ  := $(SRC:src/%.c=build/%.o)

$(EXE): $(OBJ) $(RES)
	$(CC) $(ODIN_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ODIN_CFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

build/odin_res.o: packaging/windows/odin.rc packaging/windows/odin.manifest
	@mkdir -p build
	windres -I packaging/windows $< -O coff -o $@

# unit tests (with address sanitizer where it exists, so memory bugs blow up loudly),
# then the username engine against a fake local web server (needs python3)
test: $(EXE) $(SRC)
	@mkdir -p build
	$(CC) $(ODIN_CFLAGS) $(filter-out -O2,$(CFLAGS)) -g $(SAN) $(LDFLAGS) -o build/test_odin \
		tests/test_odin.c $(filter-out src/main.c,$(SRC)) $(LDLIBS)
	./build/test_odin
	./tests/user/run.sh $(CURDIR)/$(EXE)

# throws random junk at the parsers (needs clang + libfuzzer). FUZZ_TIME is in seconds
FUZZ_TIME ?= 60
fuzz: $(SRC)
	@mkdir -p build/corpus
	clang -std=gnu11 -Isrc -g -O1 -fsanitize=fuzzer,address,undefined -o build/fuzz_odin \
		tests/fuzz_parse.c $(filter-out src/main.c,$(SRC)) $(LDLIBS)
	./build/fuzz_odin -max_total_time=$(FUZZ_TIME) -max_len=65536 build/corpus

install: $(EXE)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(EXE) $(DESTDIR)$(PREFIX)/bin/$(EXE)

clean:
	rm -rf build odin odin.exe

.PHONY: test fuzz install clean
-include $(OBJ:.o=.d)
