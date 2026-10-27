# Include variables from the shared embedded-level .env (single source for
# core and head builds) if it exists
-include ../.env
export

# Offline-first PlatformIO; interval is days.
export PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL=7
export PLATFORMIO_SETTING_ENABLE_TELEMETRY=No

.PHONY: clean build test check check-pins check-arch upload-head size-report tidy check-cppcheck check-semgrep-community test-asan coverage check-static

# Default USB port for head uploads (stable path on this bench). Override
# per invocation when needed: make upload-head PORT=/dev/cu.usbserial-XXXX.
# A pinned default beats autodetect whenever the core is attached alongside.
PORT ?= /dev/cu.usbserial-140

clean:
	pio run -e desky-head -t clean

build:
	pio run -e desky-head

# Build + upload to the head only. PORT is required so a simultaneously
# attached core is never flashed by mistake (list ports with pio device list).
# Proves the flash with hashes, same pattern as the core upload targets.
upload-head:
	@test -n "$(PORT)" || (echo "usage: make upload-head PORT=/dev/cu.usbserial-XXXX"; exit 1)
	pio run -e desky-head --upload-port $(PORT) -t upload > /tmp/desky-head-upload.log 2>&1; CODE=$$?; cat /tmp/desky-head-upload.log; test $$CODE -eq 0
	grep -q "Hash of data verified" /tmp/desky-head-upload.log && grep -q "Hard resetting" /tmp/desky-head-upload.log

# Size budget report (report-only — never fails the build).
# OTA slot is 0x1F0000 (2031616B) per app leg of partitions_ota_nospiffs.csv;
# see ../docs/static-analysis.md. Promotion to a hard gate is a documented
# follow-up, not done here.
size-report:
	pio run -e desky-head
	pio run -e desky-head-release
	@for env in desky-head desky-head-release; do \
	  bin=$$(wc -c < .pio/build/$$env/firmware.bin | tr -d ' '); \
	  slot=2031616; \
	  spare=$$((slot - bin)); \
	  pct=$$((bin * 100 / slot)); \
	  if [ $$bin -gt $$slot ]; then st="OVER BUDGET"; else st="WITHIN BUDGET"; fi; \
	  echo "$$env: firmware.bin=$$bin slot=$$slot used=$${pct}% spare=$$spare $$st"; \
	done

test:
	pio test -e native-test

# Pin lint: exact pins only.
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
	  clang-tidy src/main.cpp -- -I../shared -I../core/src || true; \
	else \
	  echo "tidy: SKIP — clang-tidy not installed (config at ../.clang-tidy)."; \
	  echo "  install: brew install llvm  |  sudo apt install clang-tidy"; \
	fi

# cppcheck via the pio frontend (backend auto-downloaded on the first online
# run). Baseline suppressions in ../tools/cppcheck-suppressions.txt.
# Warn-only: pio check exits 0 without --fail-on-defect; backend/network
# failure skips gracefully.
check-cppcheck:
	@echo "cppcheck (advisory, baseline ../tools/cppcheck-suppressions.txt):"
	@pio check -e desky-head --flags "--suppressions-list=../tools/cppcheck-suppressions.txt" || \
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

check:
	clang-format --dry-run --Werror src/main.cpp src/hal/*.h src/services/*.h src/behavior/*.h src/transport/*.h src/abilities/*/*.h include/*.h include/mcu/*.h ../shared/common/*.h ../shared/link/*.h test/*.cpp
	$(MAKE) check-pins
	$(MAKE) check-arch
	pio run -e desky-head > /tmp/check-head.log 2>&1; CODE=$$?; cat /tmp/check-head.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-head.log
	pio run -e desky-head-release > /tmp/check-head-release.log 2>&1; CODE=$$?; cat /tmp/check-head-release.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-head-release.log
	pio test -e native-test
