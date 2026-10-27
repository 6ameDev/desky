# Static analysis — Phase 0 (report-only) + Phase 1 (advisory) + task 9 (selective)

Measured 2026-10-27. Only clang-format + semgrep-arch gate `make check`;
task 9 deliberately left everything else advisory — see the Task 9 section
for the per-tool record and promotion checklist.

## Toolchain census

| Tool | Version | Status |
| --- | --- | --- |
| clang-format | 18.1.8 | present, enforced in `check` |
| semgrep | 1.182.1 | present, enforced in `check` |
| pio Core | 6.1.19 | present (prints an "obsolete" notice, harmless) |
| clang-tidy | — | missing → Phase 1: config lands, tool still absent (skip-if-missing) |
| cppcheck | — (pio backend tool-cppcheck@1.21100.241030 auto-downloaded) | Phase 1: via `pio check` frontend, advisory |
| llvm-cov | Apple LLVM 21.0.0 (via `xcrun`) | Phase 1: present, coverage renders locally |

## Phase 1 (advisory, 2026-10-27) — `make check-static`, never in `check`

All members exit 0 with tools missing or findings logged. Custom arch rules
(`arch_guardrails.yaml` + `arch_ideal.yaml`) are untouched and keep `--error`
in `make check-arch`; community rules run WITHOUT `--error`.

- clang-tidy: single config at `embedded/.clang-tidy` (covers core/head/shared
  via upward discovery — no per-firmware copies). Curated checks only:
  bugprone-*, clang-analyzer-core.*, cert-dcl*, misc-unused-*,
  readability-braces-around-statements,
  cppcoreguidelines-narrowing-conversions-only (mirrors the `-Wconversion`
  census). Excluded: wholesale cppcoreguidelines-*, modernize-* (C++
  standard unpinned in platformio.ini — confirm before enabling),
  concurrency-mt-unsafe (FreeRTOS noise). `make tidy` skips gracefully while
  the tool is absent (this bench); install: `brew install llvm` /
  `sudo apt install clang-tidy`.
- cppcheck: via the `pio check` frontend scoped to the firmware env
  (`pio check -e desky` / `-e desky-head` with
  `--flags "--suppressions-list=../tools/cppcheck-suppressions.txt"`).
  Backend auto-downloads on first online run (no network/tool install needed
  after that); offline backend failure prints the manual invocation and skips.
  Baseline 2026-10-27: desky + desky-head both "No defects found" (one
  third-party `preprocessorErrorDirective` in Adafruit libdeps + one
  `syntaxError` false positive on `portYIELD_FROM_ISR()` in shared/common
  suppressed in `tools/cppcheck-suppressions.txt` with justification each).
- Semgrep community: `p/security-audit` + `p/cwe-top-25`, no `--error`, over
  `src test ../shared`. NOTE: `p/correctness` does not exist on the registry
  (HTTP 404 as of 2026-10-27); `p/cwe-top-25` stands in as the
  correctness-adjacent pack. Baseline: 11 + 3 rules run, 0 findings on core
  src (registry needs network on first run; cached under `~/.semgrep`).
- ASan/UBSan: `[env:native-asan]` in core + head platformio.ini (extends
  native-test, `test_build_src=false` kept, `-fsanitize=address,undefined
  -fno-omit-frame-pointer -g3`). `make test-asan` per firmware. Verified
  2026-10-27 on this Mac (Apple clang 21.0.0): core 415/415 passed,
  head 145/145 passed — no Linux-CI-only caveat needed.
- Coverage (llvm-cov, no lcov): `[env:native-cov]` (extends native-test,
  `--coverage -g3 -O0` + `../tools/pio_native_cov.py` pre-script restating
  `--coverage` on the native link line — without it the link fails with
  undefined `_llvm_gcda_emit_arcs`). Exact render invocation:
  ```
  cd core && pio test -e native-cov
  cd .pio/build/native-cov && xcrun llvm-cov gcov $(find . -name '*.o')
  ```
  (`make coverage` runs both steps and greps the per-file `Lines executed`
  summary.) Spot sample 2026-10-27: shared/link/link_codec.h 84.93% of 146
  lines; expression_catalog.h partially covered — no gate, census only.

## Task 9 (selective promotion, 2026-10-27) — gating vs advisory

Nothing below changes `make check`: the gate stays format + pins + arch +
both firmware builds + host tests. Promotion here is wiring-only
(CI artifacts, install attempts) plus an explicit per-tool record:

| Tool | State | Rationale |
| --- | --- | --- |
| clang-format / semgrep-arch | GATING (in `make check`, unchanged) | zero-tolerance formatting + layer edges |
| clang-tidy | ADVISORY (skip-if-missing) | tool absent on bench and CI image; CI install is best-effort (`continue-on-error`), never a gate dependency |
| cppcheck (`pio check`) | ADVISORY | baseline "No defects found" both envs; stays warn-only (no `--fail-on-defect`) |
| semgrep-community | ADVISORY (no `--error`) | 0 findings on core src; registry needs network, skips offline |
| ASan (`native-asan`) | ADVISORY | core 415/415 + head 145/145 green locally; CI runs `test-asan` per firmware with `continue-on-error` + log artifacts |
| Coverage (llvm-cov) | REPORT-ONLY | census sample only, no gate |
| Size report | REPORT-ONLY | all envs within budget; CI uploads the log, never gates |
| `check-analyze` | REPORT-ONLY | blocked on the `-Werror` ordering issue (see below); CI uploads the log |

Promotion checklist (each line flips independently when its condition holds):
- clang-tidy: pin per CI image and promote only after a clean local baseline is triaged (same absorb-then-gate pattern as client lint/detekt).
- cppcheck: add `--fail-on-defect` only after the backend runs green in CI with suppressions committed.
- check-analyze: uncomment the live overlay only after the `-isystem`/pragma fix lands and `src/hal/*.h` builds clean.
- Coverage/size: set bounds only after N releases of stable data — never invent a threshold.
- Baselines live here: `tools/cppcheck-suppressions.txt` (hand-maintained,
  header comment inside is the record) and the root `.clang-tidy`
  (hand-maintained single config, header comment inside is the record).

CI (`.github/workflows/ci.yml`) runs `make check-static` + `make
size-report` + per-firmware `test-asan` as advisory steps
(`continue-on-error`) with log artifacts, after an attempted
clang-tidy/cppcheck install (also `continue-on-error`).

## Size budgets (measured `firmware.bin` vs OTA app-slot)

| Env | firmware.bin | OTA slot | Used | Spare | Status |
| --- | --- | --- | --- | --- | --- |
| desky | 1,212,480 | 6,553,600 | 18% | 5,341,120 | WITHIN BUDGET |
| desky-release | 1,167,328 (−45,152 vs desky) | 6,553,600 | 17% | 5,386,272 | WITHIN BUDGET |
| desky-head | 1,159,888 | 2,031,616 | 57% | 871,728 | WITHIN BUDGET |
| desky-head-release | 1,139,216 | 2,031,616 | 56% | 892,400 | WITHIN BUDGET |

Detail (`pio run -t size` memory table): desky flash code 899,212 +
data 206,480 = 1,105,692; DIRAM 191,670 / 341,760 = 56.08% (150,090 free).
desky-release flash 865,412 + 195,116 = 1,060,528; DIRAM 191,662
(150,098 free). IRAM region reads 16,384 / 16,384 (100%) — fully consumed
reserved region, watch item if IRAM-resident code grows.

## Partition tables

Core uses the Arduino default for `esp32-s3-devkitc1-n16r8`
(`default_16MB.csv`, resolved via the board JSON — `pio run -t partitions`
is unsupported, there is no such target):

```
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x640000,
app1,     app,  ota_1,   0x650000,0x640000,
spiffs,   data, spiffs,  0xc90000,0x360000,
coredump, data, coredump,0xFF0000,0x10000,
```

Each core OTA leg is 0x640000 = 6,553,600 B against a ~1.21 MB image.

Head uses the custom `head/partitions_ota_nospiffs.csv` (no SPIFFS):
each app leg is 0x1F0000 = 2,031,616 B against a ~1.16 MB image
(spare ~871,728 B, ~43% free). `ota::fitsSlot` enforces the ceiling and
the slot size mirrors `CFG_OTA_SLOT_BYTES`.

## Analyzer flags (warnings-only census)

`make check-analyze` (core) is report-only and intentionally does not run
the overlay live. Proven blocker: `PLATFORMIO_BUILD_FLAGS` entries land
BEFORE `build_src_flags -Werror` on the g++ line, so the later `-Werror`
re-promotes `-Wconversion` hits in our own `src/hal/*.h` (motor, mpu6500,
vl53l0x drivers) to hard errors, with framework `WString.h` leaking
`-Wconversion` warnings on top. Fixes for Phase 1: pass framework includes
as `-isystem`, or wrap framework headers in
`#pragma GCC diagnostic ignored "-Wconversion"`. Existing `-Werror`
semantics for current flags are untouched.

`-Wstack-usage` is deferred to Phase 1: no per-task stack budgets exist yet,
and wiring `-fstack-usage` capture plus worst-case ISR accounting is a
Phase 1 job. Phase 0 is census-only.

`-fanalyzer` build cost unmeasured; expect 2–5x when the overlay lands.

## Local runs

Firmware builds need `DESKY_LINK_SECRET` (32 hex chars). Both Makefiles
`-include ../.env`, so `set -a; . ../.env` (from `core/` or `head/`) or an
exported env var suffices. Never commit `.env`.

Regenerate this report:

```
cd embedded && make size-report     # core + head budget tables
cd core && make check-analyze       # analyzer status + manual census command
pio test -e native-test             # host tests, per firmware dir
```
