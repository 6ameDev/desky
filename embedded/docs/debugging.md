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

## WiFi Provisioning (Dual-Mode)
- **AP Identity:** SSID `desky`, 192.168.4.1, `desky.local`. AP passphrase is derived from the link secret — field units read it off the printed on-body label, never off any display.
- **Field First Boot:** Join `desky` → portal shows setup landing → enter home SSID (1–32) + pass (empty or 8–64) → STORED → reboot to join. Wrong password self-heals: STA try fails inside 15s, lands back in the recovery portal.
- **Head Must Be Linked:** Store/forget stay disabled until the head link is live (HB-pinned peer). STORED reports only after the head sequence-ACKs (7s bound over the 5×1s retry budget); timeout says retry.
- **Force Management:** Hold pair 3s across boot → straight to provisioning AP (manage landing: view/replace/forget) until reboot. No short-press STA retry; every transition reboots.
- **Recovery:** Connected-then-lost rides 120s before AP failover. Portal starts on the first observed AP, so late failover still serves.
- **Dev Serial:** `LOG_D` prints the derived passphrase on AP raise — compiled out in release (`LOG_LEVEL_INFO`), so release logs stay silent. Verify with `strings` on both ELFs: `wifi AP pass=` present in dev, absent in release.
