# third-party/tcc/Makefile — build a Linux-hosted stock tcc from the vendored
# tinycc source (see VENDOR-REV for the pinned commit + why).
#
# S2 foundation only (vms-4ba.1): this produces a plain Linux-hosted tcc
# binary with tinycc's own aarch64 codegen backend, built entirely with
# tinycc's unmodified upstream ./configure && make. No OVMX integration.
#
# Build products NEVER land in the repo: out-of-tree via tinycc's
# --source-path, defaulting to /tmp (mirrors src/imgact/test/run_dcl_native.sh).
#
# Usage:
#   make                 # configure + build into BUILD (default /tmp/tcc-build)
#   make BUILD=/tmp/foo  # custom build dir
#   make clean           # remove BUILD
#
# Must run on the target host (aarch64 Linux) — this is a native, not cross,
# build: tinycc's configure auto-selects TCC_TARGET_ARM64 from `uname -m`.
# Run inside the project's arm64 musl Alpine container (see epic vms-4ba /
# CLAUDE.md build-environment section) — never on the bare host.

SRC   := $(CURDIR)/src
BUILD ?= /tmp/tcc-build
CC    ?= gcc

.PHONY: all tcc clean
all: tcc

tcc: $(BUILD)/tcc

$(BUILD)/tcc:
	mkdir -p $(BUILD)
	cd $(BUILD) && $(SRC)/configure --source-path=$(SRC) --cc=$(CC)
	$(MAKE) -C $(BUILD) tcc
	test -x $(BUILD)/tcc
# Note: `make tcc` builds only the tcc$(EXESUF) binary target (PROGS), not
# the sibling `all` targets (TCCLIBS incl. lib/libtcc1.a's bcheck.o, which
# fails to build against current Alpine musl headers — see VENDOR-REV) or
# TCCDOCS (texi2pod/makeinfo, not installed in the build container). Not
# needed: proof harness links with gcc, not tcc's own linker/libtcc1.

clean:
	rm -rf $(BUILD)
