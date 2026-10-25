# Debugging Cheat Sheet

## Serial Capture & Flashing
- **Port Quirks:** `/dev/cu.usbserial-0001` opens at **9600 baud and resets the chip**. Scripted tools must force `baudrate=115200` with settling noise expected.
- **Safe Capture:** Use `make upload-monitor` or `make monitor-decode`. Never pipe flash commands through `head` (SIGPIPE kills esptool).
- **Proof of Flash:** Validated *only* by `Hash of data verified` + `Hard resetting`.
- **Head OTA Dev Flash:** Build desky-head for iteration, no version bump needed. Dev sidecars stamp version 0 and dev units pull same or older images.
- **Head OTA Release:** Build desky-head-release for the locked-down path. Only release advances HEAD FW VERSION, and release units pull strictly newer images, refusing version-0 dev images.

## Decoding Crashes
1. **Run Decoder:** `make monitor-decode` (uses `.pio/build/desky/firmware.elf` automatically—rebuild first if stale).
2. **Read Reset Reason:** Cross-reference the `reset=` field in the boot banner with `FaultManager::resetReasonStr()` (`PANIC`, `TASK_WDT`, `BROWNOUT`).
3. **Watchdog Rule:** If a task hangs while subscribed to the task watchdog, call `esp_task_wdt_delete()` before halting to avoid infinite panic loops.
