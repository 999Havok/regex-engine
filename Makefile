CC       ?= cc
CFLAGS   ?= -std=c99 -Wall -Wextra -Werror -O2
CPPFLAGS  = -Iinclude -Isrc
AR       ?= ar

LIB_SRC = src/bitset.c src/ast.c src/parser.c src/positions.c \
          src/dfa.c src/minimize.c src/match.c

BUILD   = build
LIB     = $(BUILD)/libregex.a
LIB_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC))

.PHONY: all lib test memcheck bench tools clean

all: lib tools

lib: $(LIB)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---- tests -----------------------------------------------------------------
# Tests use internal.h (debug accessors), so they build against the sources.
$(BUILD)/test_regex: tests/test_regex.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_regex.c $(LIB) -o $@

test: $(BUILD)/test_regex
	$(BUILD)/test_regex

# Memory checking. valgrind is the gold standard but does not support Apple
# Silicon, so we fall back to AddressSanitizer/UBSan (catches overflows and
# use-after-free) plus the macOS `leaks` tool (catches leaks). On Linux without
# valgrind, ASan + LeakSanitizer covers both.
memcheck: | $(BUILD)
	@if command -v valgrind >/dev/null 2>&1; then \
	  echo "== valgrind =="; \
	  $(CC) $(CFLAGS) $(CPPFLAGS) tests/test_regex.c $(LIB_SRC) -o $(BUILD)/test_mc; \
	  valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all $(BUILD)/test_mc; \
	elif command -v leaks >/dev/null 2>&1; then \
	  echo "== AddressSanitizer/UBSan (memory errors) =="; \
	  $(CC) -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(CPPFLAGS) tests/test_regex.c $(LIB_SRC) -o $(BUILD)/test_asan; \
	  $(BUILD)/test_asan; \
	  echo "== macOS leaks (leak detection) =="; \
	  $(CC) -std=c99 -g -O1 $(CPPFLAGS) tests/test_regex.c $(LIB_SRC) -o $(BUILD)/test_leaks; \
	  leaks --atExit -- $(BUILD)/test_leaks | tail -n 3; \
	else \
	  echo "== AddressSanitizer + LeakSanitizer =="; \
	  $(CC) -std=c99 -g -O1 -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
	    $(CPPFLAGS) tests/test_regex.c $(LIB_SRC) -o $(BUILD)/test_asan; \
	  ASAN_OPTIONS=detect_leaks=1 $(BUILD)/test_asan; \
	fi

# ---- tools -----------------------------------------------------------------
tools: $(BUILD)/regexdump

$(BUILD)/regexdump: tools/regexdump.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) tools/regexdump.c $(LIB) -o $@

# ---- benchmark -------------------------------------------------------------
# bench.c includes the system POSIX <regex.h>, which collides by name with our
# include/regex.h. So bench.c is compiled WITHOUT -Iinclude (system header wins)
# and pulls in our public header by explicit relative path. backtrack.c, the
# foil, does need our internal headers, so it is compiled separately with them.
bench: $(BUILD)/bench
	$(BUILD)/bench

$(BUILD)/backtrack.o: bench/backtrack.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c bench/backtrack.c -o $@

$(BUILD)/bench: bench/bench.c $(BUILD)/backtrack.o $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) bench/bench.c $(BUILD)/backtrack.o $(LIB) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
