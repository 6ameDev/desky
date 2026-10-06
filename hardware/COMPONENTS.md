# desky component dossiers

Research-agent input — one dossier per item. Each dossier: exact identity
(seller link / marking) + pin table (physical order) + logic voltages +
onboard parts + mechanical dims/mounting + mating connector/cable on hand.

Status per section: `draft` (gathering) / `locked` (user approved).
Firmware pin truth lives in the MCU headers, not here:
core `embedded/core/include/mcu/esp32s3_wroom1_n16r8.h`,
head `embedded/head/include/mcu/esp32cam_ov3660.h`.
Power/signal contract: `hardware/INTERFACE.md`.

## Core carrier

### 1. ESP32-S3-DEVKITC-1-N16R8 — status: locked
- Identity: Generic `ESP32-S3-WROOM-1-N16R8`, Hubtronics SKU 07688
  (https://hubtronics.in/esp32-s3-wroom1-n16r8-dev-kit), CH9102 USB-UART,
  unsoldered headers (male since soldered by user). N16R8 module marking
  confirmed in user photo 2026-10-06.
- Pin table (solder side up, USB at bottom, 2x22 @ 2.54mm):
  - Left top→bottom: `3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, 5Vin, GND`
  - Right top→bottom: `GND, TX, RX, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21, 20, 19, GND, GND`
  - Firmware GPIOs all present: 1/2/4/5/6/7/8/10/11/15/16/17/18.
    Power pins: 3V3x2, 5Vin x1, GND x3.
- Logic voltages: 3.3V GPIO, 5V in via USB / 5Vin pin.
- Onboard parts: AMS1117 3V3 reg, RGB LED (GPIO48 — hands off),
  BOOT (GPIO0) + RST buttons. Dual USB-C: right = COM/UART (CH9102),
  left = USB/OTG native (bottom-side markings).
- Mechanical: base PCB ~56-57 x 28mm (steel-scale, +/-1mm); OAL with
  antenna tab ~63-64mm; 28mm is clean edge-to-edge, no side overhang.
  Record as `PCB 56.5x28 / OAL ~63.5mm`. Clone drift (AMS1117 + dual
  USB-C + antenna tab) explains difference vs official docs — use measured
  numbers, not official.
- Mating: carrier = female 2.54mm sockets (ordered); board has male
  soldered. 1x USB-C cable for COM flashing.

### 2. DRV8833 mini module — status: locked
- Identity: Robocraze `DRV8833 2 Channel DC Motor Driver`
  (https://robocraze.com/products/drv8833-2-channel-dc-motor-driver?variant=46651197784288,
  SKU TIFMC0199), board marking `DRV8833 ULT`. Note: seller photo shows
  `J1`, user's physical unit reads `J2` — dossier keys off the physical unit.
- Pin table (6+6, back-side view as photographed, physical order):
  - Left top→bottom: `IN4, IN3, GND, VCC, IN2, IN1`
  - Right top→bottom: `EEP (=SLEEP), OUT1, OUT2, OUT3, OUT4, ULT (=VM)`
  - No nFAULT broken out. NOT the generic AIN/BIN/VM layout — use this table.
- Logic voltages: 3V/5V logic compatible (seller spec); VCC fed per wiring docs.
- VM: 2.7–10.8V, 1.2A cont / 2A peak per channel (seller spec).
  Reverse-polarity protection on-module. Package includes 12x male headers.
- Onboard parts: J2 pads OPEN (not joined) on user's unit — SLEEP strapping
  per wiring docs, not re-derived here.
- Mechanical: ~16x18mm user-measured (seller "18x8mm" disregarded as wrong).
- Mating / wiring intent: primary = short wire links from DRV socket row to
  board-side JST-XH 2-pin headers (OUT1/2 → left motor, OUT3/4 → right);
  motor side crimped plugs, no motor wire into DRV socket. Fallback = pinhole
  method (motor wire soldered into neighbour hole + wire link to socket,
  desolder to repair, add strain relief). JST-XH budget: 8 pairs total, 2x
  power inlets + 2x motors + 4x spare.

### 3. VL53L0X module — status: locked
- Identity: Robocraze `VL53L0X Laser Ranging Sensor`
  (https://robocraze.com/products/vl53l0x-laser-ranging-sensor?variant=40194438693017,
  SKU TIFSS0099), board marking `UL53LDK`.
- Pin table (non-linear 4+2 split): bottom row `VIN, GND, SCL, SDA` +
  top-right 2 pins marked `X` and `e` (label crop). Presumed `X = XSHUT`,
  `e-fragment = GPIO1` — research agent to verify against UL53LDK variant,
  not assumed.
- Logic voltages: carrier feeds VIN ← S3 3V3 (within seller 3.3–5V range).
  Sensor-side 2.8V w/ onboard LDO + level shift presumed — agent to confirm.
- Onboard parts: XSHUT pin exists but carrier leaves it unrouted
  (firmware `MCU_TOF_XSHUT=9` defined, `INTERFACE.md` §2); sensor runs on
  module pull-up — agent to confirm pull-up present.
- Mechanical: ~13x10.5mm user-measured (seller `3x2cm` disregarded);
  1x mounting hole top-left (seller review claimed 2 — unit has 1).
- Mating: 6-pin (4+2 split); header stock check in dossier 16.

### 4. MPU module — status: locked
- Identity: Robu.in `MPU-6050 Gyro sensor`
  (https://robu.in/product/mpu-6050-gyro-sensor-2-accelerometer/),
  blue GY-521-style module, right-edge print `ITG/MPU`.
  Marking says MPU-6050, silicon behaves as MPU-6500 (bench-proven: works
  with current mpu-6500 driver / invensense-imu lib) — dossier keys off
  silicon, not ink.
- Pin table (top→bottom as photographed):
  `VCC, GND, SCL, SDA, XDA, XCL, AD0, INT`. Used: first 4 only;
  XDA/XCL/INT unconnected.
- Logic voltages: carrier feeds VCC ← S3 3V3 (user-confirmed,
  bench-working; regulator dropout marginal on paper but proven in practice).
- Onboard parts: GY-521 standard (3V3 reg + I2C pull-ups presumed —
  agent to confirm from variant docs). AD0 assumed low → addr `0x68`
  (matches firmware `MCU_ADDR_MPU_PRIMARY`; underside unverified — unit is
  breadboarded under wiring, taken as working-config truth).
- Mechanical: 21x16mm (conservative user measure, matches GY-521 standard).
- Mating: 8-pin single row; shared bus0 SDA=GPIO10/SCL=GPIO11 @400kHz
  with ToF.

### 5. Cliff sensing (discrete TCRT5000, no modules) — status: locked
- Identity: Robu.in `TCRT5000 Reflective IR sensor photoelectric switch —
  pack of 5` (https://robu.in/product/tcrt5000-reflective-ir-sensor-photoelectric-switch,
  SKU 31362). Bare leaded emitter + phototransistor pairs (no module PCB,
  no comparator, no D0). 2 pairs used (front + rear), 3 spare.
  Resistors: 10kΩ + 220Ω through-hole, plenty on hand.
- Circuit per pair (standard cliff, all rails S3 3V3):
  emitter `3V3 → 220Ω → Anode`, `Cathode → GND` (If ≈ 9mA);
  detector `3V3 → 10kΩ → Collector (= ADC tap)`, `Emitter → GND`.
  Tap → S3 GPIO1 (front) / GPIO2 (rear), ADC1 WiFi-safe, 11dB atten.
  TEMPORARY GPIO13/14 (ADC2) bench wiring retired on rewire.
  Optional 100nF tap → GND if motor noise shows (stock in dossier 16).
- Lead ID: DMM diode mode — pair with ~1.0–1.3V drop = emitter (A/K);
  other pair = detector; C vs E per datasheet orientation, mark before
  soldering — research agent to confirm exact pin-1→4 map in dossier diagram.
- Mechanical: down-facing, straight leads through perfboard, trim-to-fit;
  ride height 2–5mm (under 5mm). Black heatshrink shroud around each pair
  only if ambient IR swamps it (bench-test decision).

### 6. MAX98357A amp module — status: locked
- Identity: Hubtronics `MAX98357A BGA 2.8 I2S 3W Class D Mini Mono Amplifier`
  (https://hubtronics.in/max98357a-stereo-amplifier-module, SKU 04845),
  purple PCB, `MAX 98357A I2S Amp` front / `I2S Mono Amp. vin 2.5-5.5V /
  Default 9db Gain & (L+R)/2 out` back.
- Pin table (7-pin row, left→right): `LRC, BCLK, DIN, GAIN, SD, GND, Vin`.
  LRC←I2S WS/GPIO15, BCLK←GPIO16, DIN←DOUT/GPIO17 (shared I2S @16kHz).
- Rails: Vin ← 5V star (3V3-compatible downgrade: ~1W vs ~1.8W into 8Ω);
  GND common. I2S inputs 3.3/5V tolerant.
- Straps: SD floating ((L+R)/2 mix default), GAIN floating (9dB default) —
  both revisitable; firmware sends dual-mono so channel pick is moot.
- Speaker: green 2-pin screw terminal soldered to +/− holes (4–8Ω marking);
  7-pin male header strip stock loose.
- Mechanical: 18x17mm user-measured.

### 7. INMP441 mic modules x2 — status: locked
- Identity: Voltros `INMP441 MEMS High Precision Omnidirectional Microphone
  Module I2S`
  (https://voltros.in/products/inmp441-mems-high-precision-omnidirectional-microphone-module-i2s?variant=50914767896872,
  SKU VT00762), round black PCB, mic-icon marking.
- Pin table (top row L→R / bottom row L→R as photographed):
  `SCK, WS, L/R` + `SD, VDD, GND`.
  SCK←I2S BCLK/GPIO16, WS←GPIO15, SD→DIN/GPIO18 (shared I2S @16kHz).
- Rails: VDD ← S3 3V3 both (within 1.8–3.3V spec); GND common.
- Straps: mic A L/R→GND (left), mic B L/R→3V3 (right) — user-confirmed.
- Specs: 61dBA SNR, −26dBFS, 60Hz–15kHz, 1.4mA; bottom-port facing out
  through carrier hole.
- Mechanical: Ø ~15–16mm round, through-holes, male headers soldered.

### 8. Speaker — status: locked
- Identity: Voltros `2030 Cavity Speaker Type B, 8Ω 2W`
  (https://voltros.in/products/2030-cavity-speaker-type-b-8%CF%89-2w-high-quality-sound-compact-size-2pin-ph1-25-connector-compatible-with-luckfox-pico-ultra-boards?variant=51568747446568)
  — confirms firmware assumption (8Ω 2W) exactly.
- Specs: 8Ω ±20%, 2W, 0–20kHz, −20–55°C; cavity 20x30x6.8mm;
  2-pin PH1.25 + ~120mm cable.
- Terminals: PH1.25 cut off, tinned bare ends → amp screw terminal (+/−);
  red → + convention; re-tighten after first heat cycle.
- Mounting: cavity box on carrier (adhesive/foam TBD); amp match 8Ω @5V
  ≈ 1.8W — inside rating; firmware keeps beeps quiet.

### 9. Motors + wheels — status: locked
- Identity: Robu.in `N20 6V 200RPM Micro Metal Gear-box DC Motor`
  (https://robu.in/product/n20-6v-100-rpm-micro-metal-gear-box-dc-motor-2/,
  SKU 75011) — 200 RPM confirmed (URL misnamed, spec block governs).
- Electrical: rated 6V (operating 3–6V), 40mA no-load, stall 0.67A;
  torque 0.4 kg-cm rated / 1.2 kg-cm stall.
- Driver fit: stall 0.67A vs DRV8833 1.2A cont (2A peak) — ~1.8× margin;
  VM ← 5V star (motor runs ~5/6 speed — fine). FAULT on GPIO8.
- Mechanical: 12mm can, 26mm body + 10mm axial, D-shaft 3mmx9mm,
  M3 mounts, 12g. Bare rear tabs → wires soldered direct.
- Motor leads: 26AWG to JST-XH 2-pin wired females (15cm stock);
  OUT1/2 → left, OUT3/4 → right. 26AWG drop ~0.1V @stall — negligible.
- Wheels: 30–40mm Ø incl. tyre, 3mm D-bore required; chassis sits low
  (near floor, consistent with 2–5mm cliff height). Wheel Ø is a pending
  chassis decision, not a driver constraint.

### 10. Motor-side connectors — status: locked
- Decision: JST-XH 2-pin (right-angle male on carrier + 15cm wired female
  from motor tabs). NOT screw terminals — 2.54mm pitch either way, but XH
  is pluggable for repair and matches the inlet standard.
- Screw blocks XY308 2.54mm 2-pin 6A x2 kept free — candidate for the
  5V/GND star point (dossier 11). Re-shufflable later per user.
- Full connector stock: see §16 Connectors.

### 11. Power source + star hardware — status: locked
- Battery: Witty Fox 3.7V 2000mAh Li-ion prismatic (65x44x7mm, 40g,
  built-in OV/OC/UV protection), JST 2.54mm pitch confirmed → mates XH stock.
- Charger/boost: Hubtronics `TP4056 + boost Type-C` — USB-C in 4.5–8V,
  charge 4.2V @max 1A; boost out 4.3–27V adjustable, **tuned 5.1V and
  locked (pot dabbed)** — ships ~9V default, never connect as-received.
  Discharge max 2A; 5V reference max 1.4A; quiescent ~0.5mA; OC protection yes.
- Path: battery → TP4056 B+/B− → boost 5.1V → star → taps (S3 5Vin,
  CAM 5V inlet, amp Vin, DRV ULT/VM, 2x servos).
- Star: mixed — XY308 screw blocks as 5V/GND bus backbone (boost in + DRV +
  servos, heaviest wire first); light taps (S3, CAM, amp) as soldered rail +
  JST-XH pigtails. Specifics deferred to carrier layout pass.
- Budget: 5V/1.4A booster vs worst-case multi-stall — no simultaneous
  stall-everything; ~1hr mixed runtime (2Ah×3.7V×0.85eff/5V @~0.7A).
  Charge-while-on allowed in monitor-only state; % reads high while
  charging, true % off-charger only.
- Monitor: CJMCU-219 INA219 (Robu, ordered, not on hand), 26x22mm,
  VCC 3–5.5V ← S3 3V3, I2C → bus0 GPIO10/11 (addr default 0x40/0x41,
  no clash with 0x29/0x68 — agent confirms strap on arrival).
  Placement: Option B — series in battery+ lead for SoC voltage +
  charge/discharge sign. Priority: battery % + direction; rail consumption
  optional.
- DEFERRED (INA219 adapter wiring — revisit with agent when module arrives):
  build JST-XH 2.54 M↔F adapter, meter mid-adapter on + wire, soldered +
  heatshrink; never cut battery's molded lead. Open: header/terminal type on
  arrived board, addr strap, SoC lookup table, firmware driver task (new addr
  on bus0 — not in MCU header today).

### 12. Servos x2 (MG90S) — status: locked
- Identity: Robocraze `MG90S Mini Servo Motor (180 Degree)`
  (https://robocraze.com/products/mg90s-servo-motor?variant=40192707297433),
  metal gear, 22.8x12.2x28.5mm, 13.4g each, mounting hardware included.
- Electrical: 4.8–6V ← 5V star; speed 0.10s/60° @4.8V, 0.08s @6V;
  torque 1.8/2.2 kg-cm; stall current ~0.6–0.8A each typical (agent confirms
  exact; budget 0.8A worst-case per servo — reinforces §11
  no-simultaneous-stall note).
- Signal: 3-wire JR/Futaba plug (VCC/GND/PWM) → carrier 2.54 3-pin males.
  PWM pins unassigned — firmware task (no servo pins in MCU header today;
  needs 2x LEDC channels).
- Mechanical: horn 1 → 3D-printed arm; horn 2 → printed horn/head-rotation
  mount (chassis/CAD decision, not carrier).

## Head carrier

### 13. AI Thinker ESP32-CAM — status: locked
- Identity: Robocraze `ESP32-CAM WiFi Bluetooth Module 3MP with OV3660`
  (https://robocraze.com/products/esp32-camera-module?variant=40193645707417),
  AI-Thinker layout, `ESP32-CAM` back print, ESP-32S shield, onboard PCB
  antenna + RST button (user photos both sides).
- Pin table (2x8 @ 2.54mm, both rows on headers): left
  `IO4, IO2, IO14, IO15, IO13, IO12, GND, 5V` / right
  `3V3, IO16, IO0, GND, VCC, UOR, UOT, GND`.
  Reachable on headers: 5V, GND, GPIO14/15 (head OLED SDA/SCL) ✓;
  UOR/UOT for flashing; IO0 boot-strap; 3V3 out unloaded.
- Rails: 5V ← head inlet (JST-XH per INTERFACE.md).
- Flashing: ESP32-CAM-MB board on hand (seats CAM directly, no FTDI needed).
- Mechanical: ~40x27mm; female sockets both rows on head carrier; camera
  forward; back face (microSD slot + camera FPC + flash LED) kept clear.
- Specs: 5V in, 520KB SRAM + 4MB PSRAM, 32Mbit flash, −20–85°C.

### 14. OLED SSD1306 128x64 module — status: locked
- Identity: MakerBazar `0.96inch 128x64 OLED Display Module`, blue, 4-pin
  I2C variant (https://makerbazar.in/products/0-96-inch-128x64-oled-display-module?variant=47397087412464,
  SKU 46779).
- Pin table (top silkscreen L→R as photographed — the wiring-killer record):
  `GND, VCC, SCL, SDA`. Recorded verbatim; never assume VCC-first order.
- Fit: SDA→CAM GPIO14, SCL→CAM GPIO15 (proven-booting combo per
  INTERFACE.md; GPIO12 avoided — strapping brick). Addr 0x3C typical
  (0x3D alt — agent confirms via scan).
- Rails: VCC ← CAM 3V3 (~20mA, keeps I2C at 3V3 levels); GND common.
  Spec 3.3–5V, >160° viewing.
- Mechanical: 27x24.5mm user-measured, 4x mounting holes, male headers
  soldered → female sockets on head carrier.

### 15. OV3660 camera — status: locked
- Ribbon seated in CAM's FPC connector, latch intact — nothing to break out.
  Came with the ESP32-CAM package (§13); no separate dossier fields apply.

## General stock

### 16. General stock — status: locked
- Boards: 2x Univolt dot/vero single-sided
  (https://makerbazar.in/products/general-purpose-solderable-vero-board?variant=48251196834032)
  + 2x 8x12cm universal single-sided 2.54mm
  (https://robu.in/product/8-x-12-cm-universal-pcb-prototype-board-single-sided-2-54mm-hole-pitch/)
  — 4 boards, single-sided copper (§0-compliant).
- Headers: female 1x40 strips x4 (S3 carrier sockets covered) + male 1x40
  short strips x2; plus module-soldered males (§1, §7, §14).
- Passives: 10k + 220Ω TH plenty (§5); 0.1µF 50V ceramic pack-of-10
  (https://robocraze.com/products/0-1-uf-50v-ceramic-capacitor?variant=43638848127200);
  100µF 25V electrolytic pack-of-5 (rail bulk decoupling). 4k7: NONE on hand
  — order only if bench shows flaky I2C (agent verifies per module).
- Switches + power control: 3x self-lock KFC-8X8-A (100mA — logic/mode use
  ONLY, never rail); 8x tactile 6x6x5 momentary (spare: RST/BOOT/mode ideas);
  rocker SPST 6A/250V 23mm/20mm-hole (Robocraze — rail power kill NOW,
  series with battery+). UPGRADE PATH: 12mm metal self-lock GQ12-clone
  (Robu ~₹123, 2–3A DC claim, order next time) or P-MOSFET gated by tiny
  switch. AC-rating trap noted: PBS-plastic "3A 125VAC" types are ~0.3–0.5A
  DC — never on rail.
- Wire + cable: 30AWG silicone white/blue/green/yellow x3 each (signal runs);
  22AWG + 24AWG solid black x3 each; 26AWG teflon black x3; 20AWG silicone
  red/black x1 (heavy/star runs); 4-core flat 28AWG 6m (signal bundles);
  3-core shielded 1m (audio/motor).
- Connectors (all 2026-10-06 inventory): XH 2.54mm right-angle males 2-pin x5
  (07345), 3-pin x5 (04240), 4-pin x5 (04239); XH wired 4-pin female-female
  25cm x2 (04410), 3-pin female→bare 28cm x5 (04001), 4-pin female→bare 28cm
  x2 (04002); 2-pin wired XH pairs 15cm 26AWG x8 (motor/star budget: 2x power
  inlets + 2x motors + 4x spare — see §2); small JST 2.0mm 2-pin wired 30cm
  26AWG x10-pack (different pitch — keep segregated, space-tight alternative);
  screw blocks XY308-2.54 2-pin 6A x2 (XINYA, green, 26–18AWG — star bus
  backbone, see §10/§11).
