# Include variables from .env if it exists
-include .env
export

.PHONY: clean build build-release build-sta test test-verbose check check-pins upload-monitor upload-monitor-release upload-monitor-sta upload-sta monitor-decode

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

# Verify compilation (STA test env — needs .env WIFI_SSID/WIFI_PASS, which
# make exports from the repo root; raw `pio` without sourced env fails the
# compile-time credential assert instead of boot-looping on-chip)
build-sta:
	pio run -e desky-sta

# Build, upload to ESP32, and monitor serial output (STA).
# THE blessed STA path: make exports .env verbatim (shell quotes intact, so
# spaced SSIDs survive shlex splitting in the -D flags). Do not hand-roll
# raw `pio` STA uploads.
upload-monitor-sta:
	pio run -e desky-sta -t upload > /tmp/desky-sta-upload.log 2>&1; CODE=$$?; cat /tmp/desky-sta-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-sta-upload.log && grep -q "Hard resetting" /tmp/desky-sta-upload.log
	pio device monitor

# Build + upload (STA), no monitor — for scripted captures (see docs/debugging.md).
# Same blessed env handling as upload-monitor-sta; proves the flash with hashes.
upload-sta:
	pio run -e desky-sta -t upload > /tmp/desky-sta-upload.log 2>&1; CODE=$$?; cat /tmp/desky-sta-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-sta-upload.log && grep -q "Hard resetting" /tmp/desky-sta-upload.log

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
