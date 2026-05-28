# tests/gevico/qtest/Makefile — run individual qtest cases
#
# Usage (from the build directory):
#   make -C build/tests/gevico/qtest/ run-board-g233
#   make -C build/tests/gevico/qtest/ run-gpio-basic
#   make -C build/tests/gevico/qtest/ list
#   make -C build/tests/gevico/qtest/ run-all

SHELL := bash -o pipefail
MAKEFLAGS += -rR

# This Makefile lives in build/tests/gevico/qtest/ (symlinked from source).
# Walk up to the build root so we can find meson and the emulator.
BUILD_ROOT := $(realpath $(CURDIR)/../../..)
MESON      := $(BUILD_ROOT)/pyvenv/bin/meson
TARGET     := riscv64

# All known qtest short names (without the "test-" prefix).
# Run targets are run-<short-name>, e.g. run-board-g233.
SHORT_NAMES := \
    board-g233 \
    gpio-basic \
    gpio-int \
    pwm-basic \
    wdt-timeout \
    spi-jedec \
    flash-read \
    flash-read-interrupt \
    spi-cs \
    spi-overrun \
    i2c-gpio-init \
    i2c-gpio-bitbang \
    i2c-eeprom-rw \
    i2c-eeprom-page \
    spi-rust-init \
    spi-rust-transfer \
    spi-rust-flash

# ── run-<short-name> ──────────────────────────────────────────────────
# e.g. make run-board-g233

RUN_TARGETS := $(foreach n,$(SHORT_NAMES),run-$(n))

.PHONY: $(RUN_TARGETS)
$(RUN_TARGETS): run-%:
	@echo ">>> Running qtest-$(TARGET)/test-$* ..."
	cd $(BUILD_ROOT) && $(MESON) test --no-rebuild --print-errorlogs \
	    "qtest-$(TARGET)/test-$*"

# ── run-all ────────────────────────────────────────────────────────────

.PHONY: run-all
run-all:
	@passed=0; total=0; \
	for n in $(SHORT_NAMES); do \
	    total=$$((total + 1)); \
	    if cd $(BUILD_ROOT) && $(MESON) test --no-rebuild --print-errorlogs \
	        "qtest-$(TARGET)/test-$$n" 2>/dev/null; then \
	        passed=$$((passed + 1)); \
	    fi; \
	done; \
	echo "--- $$passed/$$total tests passed ---"

# ── list ───────────────────────────────────────────────────────────────

.PHONY: list
list:
	@echo "Available run targets:"
	@for n in $(SHORT_NAMES); do echo "  run-$$n"; done

# ── help ───────────────────────────────────────────────────────────────

.PHONY: help
help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  run-<test>   Run a single qtest (e.g. run-board-g233)"
	@echo "  run-all      Run all qtests"
	@echo "  list         List available test names"
	@echo "  help         Show this help"
