# desky wiring build sheet

Point-to-point guide for both perfboard carriers. Generated from the KiCad
netlists (`make guides` → `build/*.net`); if this file ever disagrees with a
fresh netlist export, the netlist wins — re-issue the sheet.

Build principle: `INTERFACE.md` §0 (normative — hand-soldered, through-hole,
no fab). Pin tables: `COMPONENTS.md`. Schematics are the source of truth.

Wire conventions (from stock): 20AWG red/black for star + motor power;
26AWG for JST-XH motor leads (15cm stock); 30AWG silicone colors for signals;
4-core flat 28AWG for the I2S bundle; 3-core shielded optional for audio.

## Build order (both boards)

1. Star power first (heaviest wire first): boost 5.1V → XY308 bus → DRV VCC,
   servo headers, amp Vin; light taps (S3 5Vin, CAM inlet) as pigtails.
   Rocker kill switch in series with battery+.
2. Continuity + polarity check before any module is seated: every inlet
   Pin 1 = `VCC_5V`, Pin 2 = `GND`; no shorts rail-to-rail.
3. Signals point-to-point, insulated jumpers for crossovers — never solder
   bridges. Seat modules last.

## Core carrier (`desky-core`) — signal runs

| Net | From | To | Notes |
|-----|------|----|-------|
| M_IN1 | J1-4 (GPIO4) | J3-6 (IN1) | Left-motor A1 |
| M_IN2 | J1-5 (GPIO5) | J3-5 (IN2) | Left-motor A2 |
| M_IN3 | J1-6 (GPIO6) | J3-2 (IN3) | Right-motor B1 |
| M_IN4 | J1-7 (GPIO7) | J3-1 (IN4) | Right-motor B2 |
| M_FAULT | J1-12 (GPIO8) | J4-6 (ULT/FLT) | Open-drain + pull-up; live fault path |
| I2C_SDA | J1-16 (GPIO10) | J7-3 (ToF), J8-4 (MPU) | Bus0 @400kHz, star-tap not daisy-chain |
| I2C_SCL | J1-17 (GPIO11) | J7-4 (ToF), J8-3 (MPU) | Same |
| I2S_WS | J1-8 (GPIO15) | J_AMP1-1, J_MICA1-2, J_MICB1-2 | 4-core flat bundle candidate |
| I2S_BCLK | J1-9 (GPIO16) | J_AMP1-2, J_MICA1-1, J_MICB1-1 | Same bundle |
| I2S_DOUT | J1-10 (GPIO17) | J_AMP1-3 | Amp DIN only |
| I2S_DIN | J1-11 (GPIO18) | J_MICA1-4, J_MICB1-4 | Mics SD |
| TCRT_FWD | J2-4 (GPIO1) | J_MODF1-3 → module OUT (TCRT-FWD instance) | Front tap; ADC1 WiFi-safe |
| TCRT_REV | J2-5 (GPIO2) | J_MODR1-3 → module OUT (TCRT-REV instance) | Rear tap |
| EMIT_FWD | J1-19 (GPIO13) | J_MODF1-4 → module EMIT (drain, LOW=on) | Front emitter drive |
| EMIT_REV | J1-20 (GPIO14) | J_MODR1-4 → module EMIT (drain, LOW=on) | Rear emitter drive; digital so ADC2 fine |
| SERVO_ARM | J2-6 (GPIO41) | J_SA1-1 | Arm servo PWM (JR pin 1) |
| SERVO_HEAD | J2-7 (GPIO42) | J_SH1-1 | Head-tilt PWM; longer run to neck |
| MOT_L1/L2 | J4-2/J4-3 | J5-1/J5-2 | Left motor JST-XH |
| MOT_R1/R2 | J4-4/J4-5 | J6-1/J6-2 | Right motor JST-XH |

Core power: `VCC_5V` = J1-21 + J3-4 (DRV VCC) + J4-1 (SLEEP tie) + J_AMP1-7 + J_PWR1-1 + J_SA1-2 + J_SH1-2.
`VCC_3V3` = J1-1/2 + J7-1 + J8-1 + mics VDD + J_MODF1-1 + J_MODR1-1 (module rails).
`GND` = J1-22 + J2-1/21/22 + J3-3 + all module GNDs + J_MODF1-2 + J_MODR1-2.
Heaviest GND return: J3-3 (logic+motor return) → star first.

## Head carrier (`desky-head`) — signal runs

| Net | From | To | Notes |
|-----|------|----|-------|
| OLED_SDA | J_CAML1-3 (GPIO14) | J_OLED1-4 | Proven-booting combo; never GPIO12 |
| OLED_SCL | J_CAML1-4 (GPIO15) | J_OLED1-3 | Same |
| VCC_3V3 | J_CAMR1-1 (CAM out) | J_OLED1-2 | ~20mA, keeps I2C at 3V3 |
| VCC_5V | J_HPWR1-1 | J_CAML1-8 | Head inlet → CAM 5V |
| GND | J_HPWR1-2 | J_CAML1-7, J_CAMR1-4/8, J_OLED1-1 | Common |

## Deliberately open (no copper — verified by ERC no-connects)

Core J1: RST, GPIO3/46 (strapping), XSHUT-9, parked-link-12, free-13/14.
Core J2: console TX/RX, parked-link-21, USB-19/20, misc free.
J4 GAIN/SD (module-default 9dB / (L+R)/2 mix). MPU XDA/XCL/INT.
Head: CAM IO4/IO2/IO13/IO12, IO16/IO0, right-row VCC (ambiguous — see below),
UOR/UOT (MB-flashing only).

## Deferred checks (physical build / bring-up)

- DMM: CAM right-row VCC vs 5V (continuity?) — decides if it ever gets strapped.
- I2C scan on head: OLED addr 0x3C vs 0x3D (never assumed).
- 100nF TCRT tap→GND only if motor noise shows on the rails.
- INA219 adapter + firmware driver when the module arrives (bus0 0x40/0x41).
- Servo PWM firmware task (LEDC 50Hz channels for `MCU_SERVO_ARM/HEAD`).
