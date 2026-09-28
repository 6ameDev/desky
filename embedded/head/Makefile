.PHONY: clean build test check check-pins

clean:
	pio run -e desky-head -t clean

# Verify compilation (stub)
build:
	pio run -e desky-head

# No host tests yet (native-test env is a stub) — kept for shape parity.
test:
	pio test -e native-test

# Pin lint: exact pins only — no ^ ranges, no bare git URLs, no floating platform.
# (PlatformIO has no lockfile; platformio.ini IS the lockfile.)
check-pins:
	@! grep -E "@ \\^" platformio.ini
	@! grep -E "\\.git$$" platformio.ini
	@! grep -E "releases/download/stable" platformio.ini

# Full gate (stub scope): formatting + pin lint + firmware build.
check:
	clang-format --dry-run --Werror src/main.cpp
	$(MAKE) check-pins
	pio run -e desky-head
