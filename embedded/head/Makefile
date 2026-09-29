.PHONY: clean build test check check-pins

clean:
	pio run -e desky-head -t clean

# Verify compilation (face-unit: ESP32-CAM + OV3660 + OLED + talk-wire stub)
build:
	pio run -e desky-head

# Host unit tests, no hardware (Unity)
test:
	pio test -e native-test

# Pin lint: exact pins only — no ^ ranges, no bare git URLs, no floating platform.
# (PlatformIO has no lockfile; platformio.ini IS the lockfile.)
check-pins:
	@! grep -E "@ \\^" platformio.ini
	@! grep -E "\\.git$$" platformio.ini
	@! grep -E "releases/download/stable" platformio.ini

# Full gate: formatting + pin lint + firmware build (deprecation-scanned) + host tests
check:
	clang-format --dry-run --Werror src/main.cpp src/hal/*.h src/services/*.h src/middleware/*.h include/*.h include/mcu/*.h ../shared/common/*.h test/*.cpp
	$(MAKE) check-pins
	pio run -e desky-head > /tmp/check-head.log 2>&1; CODE=$$?; cat /tmp/check-head.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-head.log
	pio test -e native-test
