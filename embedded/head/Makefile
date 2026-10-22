.PHONY: clean build test check check-pins

clean:
	pio run -e desky-head -t clean

build:
	pio run -e desky-head

test:
	pio test -e native-test

# Pin lint: exact pins only.
check-pins:
	@! grep -E "@ \\^" platformio.ini
	@! grep -E "\\.git$$" platformio.ini
	@! grep -E "releases/download/stable" platformio.ini

check:
	clang-format --dry-run --Werror src/main.cpp src/hal/*.h src/services/*.h src/behavior/*.h src/transport/*.h src/abilities/*/*.h include/*.h include/mcu/*.h ../shared/common/*.h ../shared/link/*.h test/*.cpp
	$(MAKE) check-pins
	pio run -e desky-head > /tmp/check-head.log 2>&1; CODE=$$?; cat /tmp/check-head.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-head.log
	pio test -e native-test
