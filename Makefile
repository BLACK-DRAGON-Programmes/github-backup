# Makefile — GitHub Backup Script (Linux compilation / test only)
#
# This Makefile compiles the test suite on Linux. The production build
# target is Windows (MinGW-w64) — see docs/compile.md for the Windows
# compile command. Linux compilation uses non-Windows stubs for
# WinHTTP and COM functions.
#
# Usage:
#   make            — build all test binaries
#   make test       — build and run all tests
#   make clean      — remove build artifacts
#
# Spec Section 6: "Intermediate language: C (compiled with MinGW-w64
# on Windows)." This Makefile exists solely for CI/CD and developer
# convenience on Linux workstations. The actual deployment target is
# Windows.

CC       = gcc
CFLAGS   = -Wall -Wextra -Wno-unused-parameter -I src/
SRCDIR   = src
TESTDIR  = tests
BUILDDIR = build

# Source files (all compile on Linux with stubs)
SRC = $(SRCDIR)/logger.c \
      $(SRCDIR)/notify.c \
      $(SRCDIR)/console.c \
      $(SRCDIR)/config.c \
      $(SRCDIR)/network.c \
      $(SRCDIR)/backup.c

# Test binaries
TESTS = $(BUILDDIR)/test_config \
	$(BUILDDIR)/test_logger \
	$(BUILDDIR)/test_notify \
	$(BUILDDIR)/test_network \
	$(BUILDDIR)/test_backup \
	$(BUILDDIR)/test_console \
	$(BUILDDIR)/test_main

.PHONY: all test clean

all: $(TESTS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Test binaries — each links only the modules it needs
$(BUILDDIR)/test_config: $(TESTDIR)/test_config.c $(SRCDIR)/config.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/console.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_logger: $(TESTDIR)/test_logger.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/console.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_notify: $(TESTDIR)/test_notify.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/console.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_network: $(TESTDIR)/test_network.c $(SRCDIR)/network.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/console.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_backup: $(TESTDIR)/test_backup.c $(SRCDIR)/backup.c $(SRCDIR)/config.c $(SRCDIR)/network.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/console.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/test_console: $(TESTDIR)/test_console.c $(SRCDIR)/console.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/config.c $(SRCDIR)/network.c $(SRCDIR)/backup.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

# test_main #includes main.c (with GHB_TEST_BUILD guard to exclude real main()),
# so main.c is NOT listed as a separate source here.
$(BUILDDIR)/test_main: $(TESTDIR)/test_main.c $(SRCDIR)/logger.c $(SRCDIR)/notify.c $(SRCDIR)/config.c $(SRCDIR)/network.c $(SRCDIR)/backup.c $(SRCDIR)/console.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

# Run all tests and report summary
test: $(TESTS)
	@echo "=== Running all tests ==="
	@total=0; failed=0; \
	for t in $(TESTS); do \
	        echo ""; \
	        output=$$(./$$t 2>&1); \
	        rc=$$?; \
	        echo "$$output"; \
	        if [ $$rc -ne 0 ]; then failed=$$((failed + 1)); fi; \
	        passed=$$(echo "$$output" | grep -oP '\d+(?= passed)' | tail -1); \
	        fl=$$(echo "$$output" | grep -oP '\d+(?= failed)' | tail -1); \
	        if [ -n "$$passed" ]; then total=$$((total + passed)); fi; \
	        if [ -n "$$fl" ]; then failed=$$((failed + fl)); fi; \
	done; \
	echo ""; \
	echo "=== Summary: $$total passed, $$failed failed ==="

clean:
	rm -rf $(BUILDDIR)
