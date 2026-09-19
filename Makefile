# Include variables from .env if it exists
-include .env
export

.PHONY: clean build build-release test test-verbose check upload-monitor upload-monitor-release

clean:
	pio run -e desky -t clean

# Verify compilation (dev)
build:
	pio run -e desky

# Verify compilation (release)
build-release:
	pio run -e desky-release

# Build, upload to ESP32, and monitor serial output (dev)
upload-monitor:
	pio run -e desky -t upload -t monitor

# Build, upload to ESP32, and monitor serial output (release)
upload-monitor-release:
	pio run -e desky-release -t upload -t monitor

# Host unit tests, no hardware (Unity)
test:
	pio test -e native-test

# Host unit tests, verbose per-assert output
test-verbose:
	pio test -e native-test -vvv

# Full gate: formatting + both firmware builds + host tests
check:
	clang-format --dry-run --Werror src/main.cpp src/services/*.h src/hal/*.h src/middleware/*.h src/behavior/*.h include/*.h include/mcu/*.h
	pio run -e desky
	pio run -e desky-release
	pio test -e native-test
