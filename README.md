# desky

Domain-driven monorepo: microcontroller firmware, future cross-platform
client, and shared protocol contracts.

## Repository structure

```plaintext
/
├── .github/              # CI/CD workflows
├── docs/                 # Human-readable documentation & architecture diagrams
├── shared/
│   └── protocol/         # Machine-readable contracts, packet schemas, byte maps
├── embedded/             # C++ PlatformIO microcontroller firmware
│   ├── include/
│   ├── lib/
│   ├── src/
│   ├── test/
│   ├── Makefile
│   └── platformio.ini
├── client/               # Empty placeholder for future KMP / Compose project
├── .gitignore
├── AGENTS.md
└── README.md
```

## Firmware (PlatformIO)

All PlatformIO commands run inside `./embedded`:

```bash
cd embedded && pio run                 # verify compilation (dev)
cd embedded && pio run -e desky-release
cd embedded && pio test -e native-test # host Unity tests, no hardware
```

Prefer `make` targets (they encode the lessons) from the same directory:

```bash
cd embedded && make check        # gate: format + pins + both builds + host tests
cd embedded && make build
cd embedded && make test
cd embedded && make upload-monitor
```

STA WiFi setup (robot joins the home router so the laptop keeps internet):

```bash
cp embedded/.env.example embedded/.env   # fill in WIFI_SSID / WIFI_PASS
cd embedded && make build-sta
```

## Protocol contracts

See `shared/protocol/README.md` for the binary UDP packet layout
(6-byte control, 9-byte telemetry, big-endian, XOR checksum).
Normative source: `embedded/src/middleware/udp_codec.h`.

## Docs

Start at `docs/architecture/v2.md`, then `AGENTS.md`.
Serial/crash workflow: `docs/debugging.md`.
