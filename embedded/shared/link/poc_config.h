#pragma once
// UART POC knobs — Arduino-free, header-only, namespace uartpoc.
//
// Runtime tuning for the poc-comm-link spike (fragment size, pacing, baud,
// generator mode, frame rate). ASCII wire convention for CMD/RESP payloads:
//   CMD:  "SET <k> <v>" / "GET <k>" / "GET all" / "STATS"
//   RESP: "ACK <k> <v>" / "NACK <reason>"
//
// Out-of-range numerics are CLAMPED (and ACK the clamped value); only unknown
// keys, non-numeric values, and off-list bauds are NACK-rejected.

#include <stddef.h>
#include <stdint.h>

#include "uart_frame.h"

namespace uartpoc {

struct PocConfig {
  uint16_t chunk_bytes = 128;
  uint32_t pace_us = 1000;
  uint32_t baud = 115200;
  uint8_t mode = 0;
  uint16_t fps = 10;
};

enum PocMode : uint8_t { MODE_TEXT = 0, MODE_SYNTH_RAMP = 1, MODE_SYNTH_JPEG = 2, MODE_HW_CAM = 3, MODE_COUNT = 4 };

constexpr uint16_t kChunkMin = 16;
constexpr uint16_t kChunkMax = kMaxPayload;  // 1024: codec ceiling, never exceed.
constexpr uint32_t kPaceMin = 0;
constexpr uint32_t kPaceMax = 50000;
constexpr uint8_t kModeMin = 0;
constexpr uint8_t kModeMax = 3;
constexpr uint16_t kFpsMin = 1;
constexpr uint16_t kFpsMax = 30;

inline bool isValidBaud(uint32_t b) {
  return b == 9600 || b == 57600 || b == 115200 || b == 230400 || b == 460800 || b == 921600;
}

inline bool keyEq(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) {
      return false;
    }
  }
  return *a == *b;
}

// Strict unsigned decimal: digits only, no sign/space. False on empty/overflow.
inline bool parseU32(const char* s, uint32_t& out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  uint64_t v = 0;
  for (; *s != '\0'; ++s) {
    if (*s < '0' || *s > '9') {
      return false;
    }
    v = v * 10 + static_cast<uint64_t>(*s - '0');
    if (v > 0xFFFFFFFFULL) {
      return false;
    }
  }
  out = static_cast<uint32_t>(v);
  return true;
}

inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void writeStr(char* msg, size_t cap, const char* s) {
  if (msg == nullptr || cap == 0) {
    return;
  }
  size_t i = 0;
  while (i + 1 < cap && s[i] != '\0') {
    msg[i] = s[i];
    ++i;
  }
  msg[i] = '\0';
}

inline void appendU32(char* msg, size_t cap, uint32_t v) {
  if (msg == nullptr || cap == 0) {
    return;
  }
  size_t pos = 0;
  while (pos < cap && msg[pos] != '\0') {
    ++pos;
  }
  char rev[11];
  int r = 0;
  if (v == 0) {
    rev[r++] = '0';
  }
  while (v > 0) {
    rev[r++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  }
  while (r > 0 && pos + 1 < cap) {
    msg[pos++] = rev[--r];
  }
  msg[pos < cap ? pos : cap - 1] = '\0';
}

// "ACK <key> <val>" into msg (always NUL-terminated when cap > 0).
inline void writeAck(char* msg, size_t cap, const char* key, uint32_t val) {
  writeStr(msg, cap, "ACK ");
  if (msg == nullptr || cap == 0) {
    return;
  }
  size_t pos = 0;
  while (pos < cap && msg[pos] != '\0') {
    ++pos;
  }
  size_t i = 0;
  while (pos + 1 < cap && key != nullptr && key[i] != '\0') {
    msg[pos++] = key[i++];
  }
  if (pos + 1 < cap) {
    msg[pos++] = ' ';
  }
  msg[pos < cap ? pos : cap - 1] = '\0';
  appendU32(msg, cap, val);
}

// "<key> <val>" into msg (GET rendering; always NUL-terminated when cap > 0).
inline void writeKV(char* msg, size_t cap, const char* key, uint32_t val) {
  writeStr(msg, cap, "");
  if (msg == nullptr || cap == 0) {
    return;
  }
  size_t i = 0;
  size_t pos = 0;
  while (pos + 1 < cap && key != nullptr && key[i] != '\0') {
    msg[pos++] = key[i++];
  }
  if (pos + 1 < cap) {
    msg[pos++] = ' ';
  }
  msg[pos < cap ? pos : cap - 1] = '\0';
  appendU32(msg, cap, val);
}

// Apply "SET <key> <val>". True -> msg holds "ACK k v" (possibly clamped);
// false -> msg holds "NACK <reason>". msg always NUL-terminated (cap > 0).
inline bool parseSet(const char* key, const char* val, PocConfig& cfg, char* msg, size_t msgLen) {
  uint32_t v = 0;
  if (key == nullptr || !parseU32(val, v)) {
    writeStr(msg, msgLen, "NACK bad_value");
    return false;
  }
  if (keyEq(key, "chunk") || keyEq(key, "chunk_bytes")) {
    const uint32_t c = clampU32(v, kChunkMin, kChunkMax);
    cfg.chunk_bytes = static_cast<uint16_t>(c);
    writeAck(msg, msgLen, "chunk", c);
    return true;
  }
  if (keyEq(key, "pace") || keyEq(key, "pace_us")) {
    const uint32_t p = clampU32(v, kPaceMin, kPaceMax);
    cfg.pace_us = p;
    writeAck(msg, msgLen, "pace", p);
    return true;
  }
  if (keyEq(key, "baud")) {
    if (!isValidBaud(v)) {
      writeStr(msg, msgLen, "NACK bad_baud");
      return false;
    }
    cfg.baud = v;
    writeAck(msg, msgLen, "baud", v);
    return true;
  }
  if (keyEq(key, "mode")) {
    const uint32_t m = clampU32(v, kModeMin, kModeMax);
    cfg.mode = static_cast<uint8_t>(m);
    writeAck(msg, msgLen, "mode", m);
    return true;
  }
  if (keyEq(key, "fps")) {
    const uint32_t f = clampU32(v, kFpsMin, kFpsMax);
    cfg.fps = static_cast<uint16_t>(f);
    writeAck(msg, msgLen, "fps", f);
    return true;
  }
  writeStr(msg, msgLen, "NACK unknown_key");
  return false;
}

// Render one knob ("chunk 128") or all ("chunk 128 pace 1000 baud 115200 mode
// 0 fps 10"); unknown key -> "NACK unknown_key".
inline void formatGet(const PocConfig& cfg, const char* key, char* out, size_t outLen) {
  if (keyEq(key, "chunk") || keyEq(key, "chunk_bytes")) {
    writeKV(out, outLen, "chunk", cfg.chunk_bytes);
    return;
  }
  if (keyEq(key, "pace") || keyEq(key, "pace_us")) {
    writeKV(out, outLen, "pace", cfg.pace_us);
    return;
  }
  if (keyEq(key, "baud")) {
    writeKV(out, outLen, "baud", cfg.baud);
    return;
  }
  if (keyEq(key, "mode")) {
    writeKV(out, outLen, "mode", cfg.mode);
    return;
  }
  if (keyEq(key, "fps")) {
    writeKV(out, outLen, "fps", cfg.fps);
    return;
  }
  if (keyEq(key, "all")) {
    writeKV(out, outLen, "chunk", cfg.chunk_bytes);
    // Append " pace P baud B mode M fps F" via small local writer.
    const char* keys[4] = {" pace ", " baud ", " mode ", " fps "};
    const uint32_t vals[4] = {cfg.pace_us, cfg.baud, cfg.mode, cfg.fps};
    for (int k = 0; k < 4; ++k) {
      if (out == nullptr || outLen == 0) {
        return;
      }
      size_t pos = 0;
      while (pos < outLen && out[pos] != '\0') {
        ++pos;
      }
      size_t i = 0;
      while (pos + 1 < outLen && keys[k][i] != '\0') {
        out[pos++] = keys[k][i++];
      }
      out[pos < outLen ? pos : outLen - 1] = '\0';
      appendU32(out, outLen, vals[k]);
    }
    return;
  }
  writeStr(out, outLen, "NACK unknown_key");
}

}  // namespace uartpoc
