# Include variables from .env if it exists
-include .env
export

.PHONY: clean build build-release test test-verbose check check-pins upload-monitor upload-monitor-release monitor-decode

clean:
	pio run -e desky -t clean

# Verify compilation (dev)
build:
	pio run -e desky

# Verify compilation (release)
build-release:
	pio run -e desky-release

# Build, upload to ESP32, and monitor serial output (dev).
# Upload is verified (hashes + hard reset) before the monitor starts —
# a bare SUCCESS table proves the build, not the flash.
upload-monitor:
	pio run -e desky -t upload > /tmp/desky-upload.log 2>&1; CODE=$$?; cat /tmp/desky-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-upload.log && grep -q "Hard resetting" /tmp/desky-upload.log
	pio device monitor

# Build, upload to ESP32, and monitor serial output (release)
upload-monitor-release:
	pio run -e desky-release -t upload > /tmp/desky-release-upload.log 2>&1; CODE=$$?; cat /tmp/desky-release-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-release-upload.log && grep -q "Hard resetting" /tmp/desky-release-upload.log
	pio device monitor

# Serial monitor with backtrace decoding (see docs/debugging.md)
monitor-decode:
	pio device monitor --filter esp32_exception_decoder

# Host unit tests, no hardware (Unity)
test:
	pio test -e native-test

# Host unit tests, verbose per-assert output
test-verbose:
	pio test -e native-test -vvv

# Pin lint: exact pins only — no ^ ranges, no bare git URLs, no floating platform.
# (PlatformIO has no lockfile; platformio.ini IS the lockfile.)
check-pins:
	@! grep -E "@ \\^" platformio.ini
	@! grep -E "\\.git$$" platformio.ini
	@! grep -E "releases/download/stable" platformio.ini

# Full gate: formatting + pin lint + both firmware builds (deprecation-scanned) + host tests
check:
	clang-format --dry-run --Werror src/main.cpp src/services/*.h src/hal/*.h src/middleware/*.h src/behavior/*.h include/*.h include/mcu/*.h test/*.cpp
	$(MAKE) check-pins
	pio run -e desky > /tmp/check-desky.log 2>&1; CODE=$$?; cat /tmp/check-desky.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-desky.log
	pio run -e desky-release > /tmp/check-release.log 2>&1; CODE=$$?; cat /tmp/check-release.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-release.log
	pio test -e native-test
