# desky shared protocol

Placeholder for machine-readable packet schemas / byte maps shared by the
embedded firmware (`embedded/`) and the future cross-platform client
(`client/`).

Normative source of truth today: `embedded/core/src/middleware/udp_codec.h`
(pairing/auth v1 below is authoritative alongside it; firmware
`middleware/pairing.h` + client `domain/Codec.kt` mirror it exactly).
Python peers: `embedded/scripts/udp_drive.py`, `embedded/scripts/udp_telemetry.py`.

## Conventions

- Multi-byte fields are big-endian.
- Each packet ends with a 1-byte XOR checksum over all preceding bytes.
- No heap, no Arduino headers in the codec — pure byte math so host
  Unity tests and future Kotlin code can mirror it exactly.

## Control packet: App -> Robot, 6 bytes @30-50Hz

```
[0xAA | mode | throttle | steering | flags | XOR-checksum]
```

| Byte | Field    | Meaning                                              |
| ---- | -------- | ---------------------------------------------------- |
| 0    | header   | `0xAA` (`kControlHeader`)                            |
| 1    | mode     | mode byte                                            |
| 2    | throttle | center-128 byte: 128 = stop, 255 = full +, 0 = full - |
| 3    | steering | center-128 byte: 128 = centered, + = right           |
| 4    | flags    | flags byte                                           |
| 5    | checksum | XOR of bytes 0..4                                    |

Decode rejects wrong header, wrong length, or bad checksum.

## Telemetry packet: Robot -> App, 9 bytes @10-20Hz

```
[0xBB | pitchBE i16 | rollBE i16 | distBE u16 | statusFlags | XOR-checksum]
```

| Bytes | Field       | Meaning                                    |
| ----- | ----------- | ------------------------------------------ |
| 0     | header      | `0xBB` (`kTelemetryHeader`)                 |
| 1..2  | pitch       | int16 big-endian, decidegrees (deg = raw/10) |
| 3..4  | roll        | int16 big-endian, decidegrees               |
| 5..6  | distanceMm  | uint16 big-endian, millimetres              |
| 7     | statusFlags | bit0 = cliff, bit1 = driving, bits2-3 = mode (0 MANUAL, 1 AUTONOMOUS, 2 LOW_POWER, 3 EMERGENCY), bits4-7 reserved 0 |
| 8     | checksum    | XOR of bytes 0..7                           |

Decode rejects wrong header, wrong length, or bad checksum.

## Transport

- UDP port `3333` (see scripts for the registration-hello / unicast convention).
- Robot AP `desky-<last4 MAC>` + common pass = transport only (zero security).
  App uses fixed `192.168.4.1:3333`; no IP input.
- Robot failsafes to stop ~500ms after the last control datagram.

## Pairing + authenticated drive v1 (frozen by review 2026-10-15)

Two tiers, decoupled: Tier 1 ephemeral pairing code proves physical
presence; Tier 2 persistent PSK proves ownership on every drive packet.
Rotation of Tier 1 never evicts an existing bond. Single owner: a new
bond evicts the old; re-pair only via 10s button hold (clears bond).

Constants: code = 6 ASCII digits (`esp_random`, CSPRNG); single-use,
5-min window, 5 bad tries -> 30s lockout; PSK = 16 bytes (`esp_random`);
counter = 32-bit BE, starts 1, strictly increasing, persisted periodically;
`KEK = HKDF-SHA256(salt=client_nonce||server_nonce, IKM=code ASCII,
info="desky-pair-v1", L=16)`.

- `PAIR_REQ` App -> Robot, 12 bytes:
  `[0x50 | code 6B ASCII | client_nonce 4B | XOR]`
- `PAIR_OK` Robot -> App (unicast), 38 bytes:
  `[0x51 | server_nonce 4B | ct 16B | gcm_tag 16B | XOR]`
  with `ct||tag = AES-128-GCM(KEK, nonce12=server_nonce||client_nonce||
  00 00 00 00, AAD=0x51||nonces, psk)`. Code burned on send.
- `PAIR_ERR` Robot -> App, 4 bytes:
  `[0x52 | reason 1B | retry_s 1B | XOR]`
  reason: 0=bad_code, 1=locked, 2=not_in_window, 3=already_bonded.
- `DRIVE_V2` App -> Robot, 16 bytes @30-50Hz:
  `[0xAC | mode | thr | str | flags | counter 4B BE | tag 6B | XOR]`
  with `tag = first6(HMAC-SHA256(psk, counter||mode||thr||str||flags))`.

Verify order (Core 0, pre-publish): len -> header -> XOR -> bonded? ->
counter > lastSeen -> HMAC constant-time (`mbedtls_ct_memcmp`). Fail =
silent drop (500ms failsafe stops safely). Pre-bond: legacy `0xAA`
frames pin the telemetry peer only, never drive. Post-bond: legacy
drive datagrams are dropped.

Libraries only, no custom crypto: firmware `mbedtls_md_hmac` +
`mbedtls_hkdf` + `mbedtls_gcm` + `mbedtls_ct_memcmp` + `esp_random`;
client `dev.whyoleg.cryptography:0.6.0` (JCA/CryptoKit backends).
Shared test vector (fixed code/nonces/psk -> KEK/ct/tag) must pass both
sides; vector values land in Task 6.
