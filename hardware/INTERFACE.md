# desky hardware interface contract

Cross-board contract for `hardware/core/desky-core` and `hardware/head/desky-head`.
Firmware pin truth lives in the MCU headers, not here:
core `embedded/core/include/mcu/esp32s3_wroom1_n16r8.h`,
head `embedded/head/include/mcu/esp32cam_ov3660.h`.

## 0. Guiding principle — MUST read, do NOT deviate

Hand-soldered build on plain perfboard (single-sided preferred), through-hole
components only. No PCB fab, no SMD. Every agent working on desky hardware
follows this — no exceptions without an explicit instruction change here.
(If a fabbed PCB is ever wanted, this section is the single place to flip:
rewrite it, and all downstream work follows. Autorouting via Freerouting
(`export_dsn` → `autoroute` → `import_ses`, already in the MCP `autoroute`
tools) is the option to explore at that point — not before.)

## 1. Power — common rail, star topology

One common 5V/GND rail feeds everything. The S3 does **not** power the CAM.

- Taps (each home-runs to the star point): desky-core inlet, desky-head inlet,
  motors/servos directly.
- Net names everywhere: `VCC_5V`, `GND`.
- Power inlet on **both** carriers, identical:
  `Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical`,
  Pin 1 = `VCC_5V`, Pin 2 = `GND`.

## 2. Signals — wired talk-wire PARKED, XSHUT unrouted

- **No wired UART copper on either board.** The former map
  (core TX=GPIO12/RX=GPIO21 ↔ CAM TX=GPIO12/RX=GPIO13 @115200) is parked.
  Direction is wireless (TBD); head `WiFi OFF` rule is under review for that.
  Firmware link code (`link_stub.h`, `CFG_LINK_*`, `MCU_LINK_*`) is retained
  for switch-back, never routed.
- **ToF XSHUT unrouted on core.** Firmware defines `MCU_TOF_XSHUT=9` but the
  carrier leaves it open; the sensor runs on its module pull-up.

Crucial findings kept: head OLED is SDA=GPIO14/SCL=GPIO15 (proven booting);
SDA on GPIO12 bricks boot (pull-up vs MTDI strapping, 2026-09-28).

## 3. Joint verification checklist (per board)

1. ERC clean (`make check` in `hardware/`).
2. Wire-list review: netlist vs wiring table (`hardware/WIRING.md` when nets
   land) — every joint point-to-point, insulated jumpers for crossovers,
   never solder bridges.
3. Manual-soldering pass per §0: 2.54 mm pitch, components on the
   plain side / joints on the copper side, power star home-runs first in
   heavier wire.
4. Power-polarity continuity check: both inlets Pin 1 = `VCC_5V`,
   Pin 2 = `GND`; no TX/RX copper present while parked.
