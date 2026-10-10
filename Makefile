CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -g -O0
INCLUDES = -Iinclude -Itests
LDFLAGS = -pthread -lm

BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin
TEST_BIN_DIR = $(BUILD_DIR)/tests

# Sources
SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c, $(BUILD_DIR)/src/%.o, $(SRCS))

# Tools
TOOLS = $(wildcard tools/*.c)
TOOL_BINS = $(patsubst tools/%.c, $(BIN_DIR)/%, $(TOOLS))

# Test sources
TEST_SRCS = $(wildcard tests/test_*.c) tests/diff_posix.c
# Exclude test_failing_dummy from standard test suite
STD_TEST_SRCS = $(filter-out tests/test_failing_dummy.c, $(TEST_SRCS))
TEST_BINS = $(patsubst tests/%.c, $(TEST_BIN_DIR)/%, $(STD_TEST_SRCS))

all: dirs $(OBJS) $(TOOL_BINS)

dirs:
	@mkdir -p $(BUILD_DIR)/src $(BIN_DIR) $(TEST_BIN_DIR) reports

$(BIN_DIR)/%: tools/%.c $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN_DIR)/%: tests/%.c $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(OBJS) -o $@ $(LDFLAGS)

test: all $(TEST_BINS)
	@echo "=== Running Minifs Test Suite ==="
	@for t in $(TEST_BINS); do \
		echo "Running $$t..."; \
		$$t || exit 1; \
	done
	@echo "Running E2E integration test..."
	@./scripts/e2e.sh
	@echo "=== All Tests Passed Successfully ==="

test-asan:
	@echo "Running tests with AddressSanitizer (if supported)..."
	$(MAKE) test CFLAGS="$(CFLAGS) -fsanitize=address,undefined"

test-valgrind: dirs $(TEST_BINS)
	@echo "Running tests under Valgrind (if installed)..."
	@if command -v valgrind >/dev/null 2>&1; then \
		for t in $(TEST_BINS); do \
			valgrind --leak-check=full --error-exitcode=1 $$t || exit 1; \
		done; \
	else \
		echo "Valgrind not found on system, skipped."; \
	fi

gate:
	@if [ -z "$(N)" ]; then echo "Usage: make gate N=<module_num>"; exit 1; fi
	./scripts/run_gate.sh $(N)

bench:
	@echo "Benchmarks will be run via scripts/bench.sh"
	./scripts/bench.sh

coverage:
	@echo "Coverage run (requires gcov/lcov)"
	$(MAKE) clean
	$(MAKE) test CFLAGS="$(CFLAGS) --coverage"
	@mkdir -p reports/coverage
	gcov $(SRCS) || true

clean:
	rm -rf $(BUILD_DIR) *.img *.log /tmp/minifs_test*

.PHONY: all dirs test test-asan test-valgrind gate bench coverage clean
