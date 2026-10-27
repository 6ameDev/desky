# Include variables from the shared embedded-level .env (single source for
# core and head builds) if it exists
-include ../.env
export

# Offline-first PlatformIO; interval is days.
export PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL=7
export PLATFORMIO_SETTING_ENABLE_TELEMETRY=No

.PHONY: clean build build-release build-bench test test-verbose check check-pins check-arch upload-monitor upload-monitor-release upload-bench monitor-decode size-report check-analyze tidy check-cppcheck check-semgrep-community test-asan coverage check-static

# Default USB port for core uploads/monitors (stable modem path on this
# bench). Override per invocation when needed: make upload-monitor
# PORT=/dev/cu.usbserial-XXXX. A pinned default beats autodetect whenever
# the head is attached alongside.
PORT ?= /dev/cu.usbmodem5C941480511

clean:
	pio run -e desky -t clean

# Verify compilation (dev)
build:
	pio run -e desky

# Verify compilation (release)
build-release:
	pio run -e desky-release

# Build, upload to ESP32, and monitor serial output (dev).
# PORT is required so a simultaneously attached head is never flashed by
# mistake (list ports with pio device list). Upload is verified (hashes +
# hard reset) before the monitor starts — a bare SUCCESS table proves the
# build, not the flash.
upload-monitor:
	@test -n "$(PORT)" || (echo "usage: make upload-monitor PORT=/dev/cu.usbserial-XXXX"; exit 1)
	pio run -e desky --upload-port $(PORT) -t upload > /tmp/desky-upload.log 2>&1; CODE=$$?; cat /tmp/desky-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-upload.log && grep -q "Hard resetting" /tmp/desky-upload.log
	pio device monitor --port $(PORT)

# Build, upload to ESP32, and monitor serial output (release)
upload-monitor-release:
	@test -n "$(PORT)" || (echo "usage: make upload-monitor-release PORT=/dev/cu.usbserial-XXXX"; exit 1)
	pio run -e desky-release --upload-port $(PORT) -t upload > /tmp/desky-release-upload.log 2>&1; CODE=$$?; cat /tmp/desky-release-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-release-upload.log && grep -q "Hard resetting" /tmp/desky-release-upload.log
	pio device monitor --port $(PORT)

# Verify compilation (bench env: WiFi off for TCRT bring-up).
# Deliberately NOT part of `check` — the gate stays desky/desky-release.
build-bench:
	pio run -e desky-bench

# Build + upload (bench), no monitor — for scripted TCRT calibration captures
# (see docs/debugging.md). Same hash-verified upload pattern as the dev path.
upload-bench:
	@test -n "$(PORT)" || (echo "usage: make upload-bench PORT=/dev/cu.usbserial-XXXX"; exit 1)
	pio run -e desky-bench --upload-port $(PORT) -t upload > /tmp/desky-bench-upload.log 2>&1; CODE=$$?; cat /tmp/desky-bench-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-bench-upload.log && grep -q "Hard resetting" /tmp/desky-bench-upload.log

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

# Architecture guardrails (see ../tools/arch_guardrails.yaml): layer edges as
# Semgrep ERROR rules + tracked-source hygiene (no task breadcrumbs, format
# glob coverage over src/**/*.h, one test runner main). Semgrep pinned in CI.
check-arch:
	semgrep --config ../tools/arch_guardrails.yaml --error src test
	@echo "Phase 2 ideal-layer rules (arch_ideal.yaml), enforced as errors since P2.3."
	semgrep --config ../tools/arch_ideal.yaml --error src test
	@! git --no-pager grep -nE "SLICE|Slice [0-9]|Task [0-9]|Task T-[0-9]|T-[0-9]+|H[0-9]\.[0-9]|M[0-9]+-M[0-9]+|flag-day|FLAG-DAY|Pre-2|PRE-2|middleware|MIDDLEWARE" -- src test
	@for f in $$(find src -name '*.h'); do \
	  case "$$f" in \
	    src/services/*.h|src/hal/*.h|src/behavior/*.h|src/transport/*.h|src/abilities/*/*.h) ;; \
	    *) echo "check-arch: $$f not covered by the format globs"; exit 1;; \
	  esac; \
	done
	@test $$(grep -l "int main" test/*.cpp | wc -l) -eq 1

# Size budget report (report-only — never fails the build).
# OTA slot is 0x640000 (6553600B) per app leg of default_16MB.csv; see
# ../docs/static-analysis.md for the full budget table. Promotion to a hard
# gate is a documented follow-up, not done here.
size-report:
	pio run -e desky
	pio run -e desky-release
	@for env in desky desky-release; do \
	  bin=$$(wc -c < .pio/build/$$env/firmware.bin | tr -d ' '); \
	  slot=6553600; \
	  spare=$$((slot - bin)); \
	  pct=$$((bin * 100 / slot)); \
	  if [ $$bin -gt $$slot ]; then st="OVER BUDGET"; else st="WITHIN BUDGET"; fi; \
	  echo "$$env: firmware.bin=$$bin slot=$$slot used=$${pct}% spare=$$spare $$st"; \
	done

# Analyzer flags, warnings-only census (report-only — NOT in `check`).
# Status: the PLATFORMIO_BUILD_FLAGS overlay below does NOT run clean, so the
# live command stays commented until the ordering blocker is fixed (TODO).
# Proven 2026-10-27: env build_flags land BEFORE build_src_flags -Werror on
# the g++ line, so the later -Werror re-promotes -Wconversion hits in our own
# src/hal/*.h to hard errors; framework WString.h leaks -Wconversion warnings
# on top. Fixes: pass framework includes as -isystem, or wrap framework
# headers in `#pragma GCC diagnostic ignored "-Wconversion"`.
# TODO: uncomment the live overlay once the -isystem/pragma fix lands;
# keep -Werror semantics for existing flags.
# PLATFORMIO_BUILD_FLAGS='-Wconversion -Wdouble-promotion -fanalyzer' pio run -e desky
check-analyze:
	@echo "check-analyze (report-only): analyzer overlay blocked, see TODO above."
	@echo "Manual census: PLATFORMIO_BUILD_FLAGS='-Wconversion -Wdouble-promotion -fanalyzer' pio run -e desky"

# Static analysis (ALL report-only — NOT in `check`). Every check-static
# member exits 0 with tools missing or findings logged, never failing the
# build. Custom arch rules keep --error in check-arch (untouched); community
# rules below run WITHOUT --error.

# clang-tidy census (curated checks in ../.clang-tidy, shared root covering
# core/head/shared). Tool absent on this bench — skips gracefully with
# install notes. If present, the run is warn-only (|| true).
tidy:
	@if command -v clang-tidy >/dev/null 2>&1; then \
	  clang-tidy --version; \
	  echo "tidy (advisory): findings logged, never failing:"; \
	  clang-tidy src/main.cpp -- -I../shared -I../head/include -I../head/src || true; \
	else \
	  echo "tidy: SKIP — clang-tidy not installed (config at ../.clang-tidy)."; \
	  echo "  install: brew install llvm  |  sudo apt install clang-tidy"; \
	fi

# cppcheck via the pio frontend (backend auto-downloaded on the first online
# run — verified 2026-10-27 as tool-cppcheck@1.21100.241030). Baseline
# suppressions in ../tools/cppcheck-suppressions.txt. Warn-only: pio check
# exits 0 without --fail-on-defect; backend/network failure skips gracefully.
check-cppcheck:
	@echo "cppcheck (advisory, baseline ../tools/cppcheck-suppressions.txt):"
	@pio check -e desky --flags "--suppressions-list=../tools/cppcheck-suppressions.txt" || \
	  (echo "check-cppcheck: SKIP — pio check backend unavailable offline."; \
	   echo "  manual: cppcheck --enable=warning,style,performance,portability,unusedFunction --suppressions-list=../tools/cppcheck-suppressions.txt src ../shared"; \
	   true)

# Semgrep community rules ALONGSIDE the custom arch rules (untouched, still
# --error in check-arch). Runs WITHOUT --error so check-arch stays green.
# NOTE: p/correctness does not exist on the registry (404 as of 2026-10-27);
# p/cwe-top-25 stands in as the correctness-adjacent pack. Needs network on
# first run (rules cache under ~/.semgrep); unreachable registry skips.
check-semgrep-community:
	@echo "semgrep-community (advisory, no --error): p/security-audit + p/cwe-top-25 (p/correctness 404s, see above)"
	@semgrep --config p/security-audit --config p/cwe-top-25 src test ../shared || \
	  (echo "check-semgrep-community: SKIP — registry unreachable offline."; true)

# Sanitizer run of the host tests ([env:native-asan]). Strict: real test run.
test-asan:
	pio test -e native-asan

# Coverage run ([env:native-cov], gcov-style). Renders with the Apple-shipped
# llvm-cov — no lcov install. See ../docs/static-analysis.md.
coverage:
	pio test -e native-cov
	@if command -v xcrun >/dev/null 2>&1 && xcrun llvm-cov --version >/dev/null 2>&1; then \
	  cd .pio/build/native-cov && xcrun llvm-cov gcov $$(find . -name '*.o') 2>/dev/null | grep -E "^File|executed" | grep -v Creating; \
	else \
	  echo "coverage: tests ran; SKIP render — xcrun llvm-cov not installed."; \
	fi

check-static: tidy check-cppcheck check-semgrep-community
	@echo "check-static (advisory): log above; never fails the build."

# Full gate: formatting + pin lint + arch guardrails + both firmware builds (deprecation-scanned) + host tests
check:
	clang-format --dry-run --Werror src/main.cpp src/services/*.h src/hal/*.h src/behavior/*.h src/transport/*.h src/abilities/*/*.h include/*.h include/mcu/*.h ../shared/common/*.h ../shared/link/*.h test/*.cpp
	$(MAKE) check-pins
	$(MAKE) check-arch
	pio run -e desky > /tmp/check-desky.log 2>&1; CODE=$$?; cat /tmp/check-desky.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-desky.log
	pio run -e desky-release > /tmp/check-release.log 2>&1; CODE=$$?; cat /tmp/check-release.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-release.log
	pio test -e native-test
