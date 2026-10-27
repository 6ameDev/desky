# Static analysis — Phase 0 (report-only)

Measured 2026-10-27. Nothing here gates `make check`; promotion to hard
gates happens in task 9.

## Toolchain census

| Tool | Version | Status |
| --- | --- | --- |
| clang-format | 18.1.8 | present, enforced in `check` |
| semgrep | 1.182.1 | present, enforced in `check` |
| pio Core | 6.1.19 | present (prints an "obsolete" notice, harmless) |
| clang-tidy | — | missing |
| cppcheck | — | missing |
| llvm-cov | — | missing |

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
