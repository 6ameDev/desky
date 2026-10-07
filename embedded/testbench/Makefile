.PHONY: clean build test check check-pins upload-monitor monitor-decode

clean:
	pio run -e desky-testbench -t clean

# Verify compilation (testbench: ESP32 DevKit v1 + banner + I2C scan + blink)
build:
	pio run -e desky-testbench

# Host unit tests, no hardware (Unity)
test:
	pio test -e native-test

# Pin lint: exact pins only — no ^ ranges, no bare git URLs, no floating platform.
# (PlatformIO has no lockfile; platformio.ini IS the lockfile.)
check-pins:
	@! grep -E "@ \\^" platformio.ini
	@! grep -E "\\.git$$" platformio.ini
	@! grep -E "releases/download/stable" platformio.ini

# Build, upload to ESP32, and monitor serial output.
# Upload is verified (hashes + hard reset) before the monitor starts —
# a bare SUCCESS table proves the build, not the flash.
upload-monitor:
	pio run -e desky-testbench -t upload > /tmp/testbench-upload.log 2>&1; CODE=$$?; cat /tmp/testbench-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/testbench-upload.log && grep -q "Hard resetting" /tmp/testbench-upload.log
	pio device monitor

# Serial monitor with backtrace decoding (see docs/debugging.md)
monitor-decode:
	pio device monitor --filter esp32_exception_decoder

# Full gate: formatting + pin lint + firmware build (deprecation-scanned) + host tests
check:
	clang-format --dry-run --Werror src/main.cpp include/*.h include/mcu/*.h ../shared/common/*.h test/*.cpp
	$(MAKE) check-pins
	pio run -e desky-testbench > /tmp/check-testbench.log 2>&1; CODE=$$?; cat /tmp/check-testbench.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-testbench.log
	pio test -e native-test
