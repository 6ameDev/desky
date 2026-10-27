# Include variables from the shared embedded-level .env (single source for
# core and head builds) if it exists
-include ../.env
export

# Offline-first PlatformIO; interval is days.
export PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL=7
export PLATFORMIO_SETTING_ENABLE_TELEMETRY=No

.PHONY: clean build test check check-pins check-arch upload-head size-report

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

# Size budget report (Phase 0, report-only — never fails the build).
# OTA slot is 0x1F0000 (2031616B) per app leg of partitions_ota_nospiffs.csv;
# see ../docs/static-analysis.md. Promotion to a hard gate in task 9, not here.
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

check:
	clang-format --dry-run --Werror src/main.cpp src/hal/*.h src/services/*.h src/behavior/*.h src/transport/*.h src/abilities/*/*.h include/*.h include/mcu/*.h ../shared/common/*.h ../shared/link/*.h test/*.cpp
	$(MAKE) check-pins
	$(MAKE) check-arch
	pio run -e desky-head > /tmp/check-head.log 2>&1; CODE=$$?; cat /tmp/check-head.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-head.log
	pio run -e desky-head-release > /tmp/check-head-release.log 2>&1; CODE=$$?; cat /tmp/check-head-release.log; test $$CODE -eq 0
	@! grep -i "is deprecated and will be removed" /tmp/check-head-release.log
	pio test -e native-test
