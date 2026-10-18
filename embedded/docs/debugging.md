# Debugging desky firmware

## Serial capture rules (read before touching USB)

- The port (`/dev/cu.usbserial-0001`) opens at **9600 baud by default**.
  Scripted captures must set 115200 explicitly (pyserial `baudrate=`).
  Prefer `make upload-monitor` / `make monitor-decode`, which set it.
- **Opening the port resets the chip** (DTR auto-reset). Expect settling
  noise plus a fresh boot at every capture head (~200ms); do not conclude
  "it just booted" from open-adjacent output.
- `pio device monitor` needs a TTY; piped runs die in miniterm. Drive it
  under a pty or use pyserial for scripted capture.
- Never pipe a flash command through `head` — SIGPIPE kills esptool
  mid-write and strands the chip in download mode. Redirect to a file,
  then grep (the make upload targets already do this).
- Flash is proven only by `Hash of data verified` + `Hard resetting`,
  never by the SUCCESS table alone.

## Decoding a crash (standard flow, works today)

1. Reproduce the crash with the monitor attached:
   `make monitor-decode` (`pio device monitor --filter esp32_exception_decoder`)
   The filter uses `.pio/build/desky/firmware.elf` automatically — rebuild
   first if the flashed firmware is stale, or frames decode against the
   wrong binary.
2. A panic prints `Guru Meditation` / `Backtrace:` frames; the filter
   appends `file:line` to each frame. Copy the decoded frames into the
   bug report along with the preceding log context.
3. Read the `reset=` field of the next `BOOT` banner and cross-reference
   `FaultManager::resetReasonStr()` (`PANIC`, `TASK_WDT`, `BROWNOUT`, ...):
   it tells you which supervisor fired, independent of the backtrace.
   Note `ESP.restart()` always transits the ROM bootloader, so ROM text +
   `rst:0xc` on a reboot cycle is normal — panic-vs-restart is decided by
   the presence of a panic dump, never by ROM silence.

## Healthy baselines (regress against these)

- Idle heartbeat: `stack_wm=6092 heap_free=255360 largest=110580`, stable
  across 10s+ (drift = leak or fragmentation — investigate).
- Skeleton footprint: Flash 29.7% / RAM 9.1% (`pio run` summary).
- Revisit partitions when `firmware.bin` exceeds 1MB (see `platformio.ini`
  tripwire note); the default table already holds a 64KB `coredump`
  partition for the spike below.

## What this does not cover (near-future spike)

Core dumps are **not yet enabled**: our Test-3 watchdog panics printed a
backtrace with no coredump lines, so this Arduino SDK build almost
certainly ships with coredump disabled. The default partition table
already reserves a 64KB `coredump` partition — only the enable flag is
missing. Pick up the spike soon: confirm the SDK default, find the
pioarduino-supported way to enable flash coredump (or prove it needs SDK
surgery and defer), and document the `esp-coredump.py` decode flow here.
Backtraces suffice while crashes are clean; coredump earns its keep once
heap corruption starts lying to backtraces.
