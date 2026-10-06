# desky v2 monorepo orchestrator (task 1).
#
# Firmwares live in per-board roots, each with its own PlatformIO project
# and Makefile:
#   core/ — desky-core, primary brain (ESP32-S3-WROOM-1-N16R8).
#   head/ — desky-head, face-unit stub (ESP32-CAM, empty shell until task 2).
#   shared/link/ — core<->head link codec (PARKED placeholder).
#
# Run per-firmware work inside its dir (e.g. `cd core && make check`).
# This file only forwards, so `make check` from embedded/ still gates all.

.PHONY: check check-all check-core check-head build build-all build-core build-head test-core clean-core

check: check-all

check-all: check-core check-head

build: build-all

build-all: build-core build-head

check-core:
	$(MAKE) -C core check

check-head:
	$(MAKE) -C head check

build-core:
	$(MAKE) -C core build

build-head:
	$(MAKE) -C head build

test-core:
	$(MAKE) -C core test

clean-core:
	$(MAKE) -C core clean
