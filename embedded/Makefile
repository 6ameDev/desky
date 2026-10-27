# desky v2 monorepo orchestrator (task 1).
#
# Firmwares live in per-board roots, each with its own PlatformIO project
# and Makefile:
#   core/ — desky-core, primary brain (ESP32-S3-WROOM-1-N16R8).
#   head/ — desky-head, face-unit stub (ESP32-CAM, empty shell until task 2).
#   testbench/ — desky-testbench, component POC rig (ESP32 DevKit v1).
#   shared/link/ — core<->head link codec (PARKED placeholder).
#
# Run per-firmware work inside its dir (e.g. `cd core && make check`).
# This file only forwards, so `make check` from embedded/ still gates all.

.PHONY: check check-all check-core check-head check-testbench build build-all build-core build-head build-testbench test-core clean-core setup size-report check-static help

# Target pointers (see client/ARCHITECTURE.md section 5 + docs/static-analysis.md
# Task 9 section for the gating-vs-advisory record).
help:
	@echo "check            firmware gates (format + pins + arch + builds + host tests)"
	@echo "check-static     advisory rollup (tidy + cppcheck + semgrep-community, never gating)"
	@echo "size-report      OTA budget census (report-only, never gating)"
	@echo "setup            pinned gate tooling (semgrep + clang-format)"

# One-time tooling for gates (pinned to CI): pipx install pipx first if needed.
setup:
	pipx install semgrep==1.182.1
	pipx install clang-format==18.1.8
	# Phase 1 static analysis (NOT pinned — advisory only, never gating):
	#   cppcheck: prefer the pio frontend (`make check-cppcheck` auto-downloads
	#     tool-cppcheck on first online run); standalone: brew install cppcheck
	#     (currently unpinned — pin when a CI image needs it) or
	#     sudo apt install cppcheck.
	#   clang-tidy: brew install llvm (ships clang-tidy) or
	#     sudo apt install clang-tidy (pin per CI image when promoting in
	#     task 9). Config lives at embedded/.clang-tidy.
	#   CI hint: keep the two pins above exact; add cppcheck/clang-tidy pins
	#     to the CI image (not here) when task 9 promotes them to gates.

check: check-all

check-all: check-core check-head check-testbench

build: build-all

build-all: build-core build-head build-testbench

check-core:
	$(MAKE) -C core check

check-head:
	$(MAKE) -C head check

check-testbench:
	$(MAKE) -C testbench check

build-core:
	$(MAKE) -C core build

build-head:
	$(MAKE) -C head build

build-testbench:
	$(MAKE) -C testbench build

test-core:
	$(MAKE) -C core test

clean-core:
	$(MAKE) -C core clean

# Phase 0 size budget census (report-only, never gating).
size-report:
	$(MAKE) -C core size-report
	$(MAKE) -C head size-report

# Phase 1 static analysis rollup (report-only, never gating — promotion is
# task 9, NOT here). Forwards to core + head; every member exits 0.
check-static:
	$(MAKE) -C core check-static
	$(MAKE) -C head check-static
