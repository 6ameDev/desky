#pragma once
// UART POC synthetic payloads — Arduino-free, header-only, single source of
// truth (embedded/shared/link, used by both Head and S3 firmwares + host tests).
//
// Deterministic generator payloads shared by the streaming generator and the
// SELFTEST verifier: the verifier regenerates EXACTLY what the generator emits
// from (mode, fid, off, n, total) and memcmps.
// Modes: 0 TEXT (512B of 32B "TXT fid:off" lines), 1 RAMP (1024B incrementing
// bytes), 2 SYNTH_JPEG (2048B xorshift32 + SOI/EOI markers only — NOT
// decodable JPEG by design). Mode 3 is real camera data (no synthetic).
//
// No heap, no Arduino, no String. Unit-tested in test/test_uart_frame.cpp.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "link/uart_frame.h"

namespace pocself {

constexpr uint32_t kTextTotal = 512;
constexpr uint32_t kRampTotal = 1024;
constexpr uint32_t kJpegTotal = 2048;

// Encoded-byte feed slice for SELFTEST MEM (unaligned-delivery coverage).
constexpr size_t kSelftestSlice = 7;

// Source byte count for a synthetic mode; 0 for mode 3+ (camera/unknown,
// never a synthetic source).
inline uint32_t synthTotal(uint8_t mode) {
  if (mode == 0) {
    return kTextTotal;
  }
  if (mode == 1) {
    return kRampTotal;
  }
  if (mode == 2) {
    return kJpegTotal;
  }
  return 0;
}

// CHUNK flags for a synthetic mode (SYNTHETIC set for modes 1/2 only).
inline uint8_t synthFlags(uint8_t mode) {
  if (mode == 1 || mode == 2) {
    return uartpoc::FLAG_SYNTHETIC;
  }
  return 0;
}

// Fill dst[0..n) with the deterministic source bytes at absolute offset off
// of frame fid (total = synthTotal(mode)). Pure in (mode, fid, off, n,
// total): two calls with the same args yield identical bytes.
inline void fillSynthetic(uint8_t mode, uint16_t fid, uint32_t off, uint8_t* dst, uint16_t n, uint32_t total) {
  if (dst == nullptr) {
    return;
  }
  if (mode == 1) {
    for (uint16_t i = 0; i < n; ++i) {
      dst[i] = static_cast<uint8_t>((off + i) & 0xFF);
    }
    return;
  }
  if (mode == 2) {
    // Xorshift32 regenerated per chunk (deterministic in offset); SOI/EOI
    // markers only — payload is NOT decodable JPEG by design.
    uint32_t x = static_cast<uint32_t>(fid) * 2654435761UL + 1;
    if (x == 0) {
      x = 1;
    }
    for (uint32_t i = 0; i < off + n; ++i) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      if (i >= off) {
        dst[i - off] = static_cast<uint8_t>(x & 0xFF);
      }
    }
    if (off == 0 && n >= 2) {
      dst[0] = 0xFF;
      dst[1] = 0xD8;
    }
    if (off + n >= total && n >= 2) {
      dst[n - 2] = 0xFF;
      dst[n - 1] = 0xD9;
    }
    return;
  }
  // MODE_TEXT: incrementing 32B lines "TXT fid:off " + alpha filler.
  for (uint16_t i = 0; i < n;) {
    char head[24];
    const uint32_t lineOff = off + i;
    snprintf(head, sizeof(head), "TXT %04u:%04u ", static_cast<unsigned>(fid), static_cast<unsigned>(lineOff));
    uint16_t h = 0;
    while (head[h] != '\0' && i < n) {
      dst[i++] = static_cast<uint8_t>(head[h++]);
    }
    while (i < n && ((off + i) % 32) != 31) {
      dst[i] = static_cast<uint8_t>('a' + ((off + i) % 26));
      ++i;
    }
    if (i < n) {
      dst[i++] = '\n';
    }
  }
}

}  // namespace pocself
