#pragma once
// QR V1-L bitmap encoder: 6 ASCII digits -> 21x21 modules. Fixed params, no config keys.

namespace qr {

// QR Version 1 edge length in modules.
constexpr int kSize = 21;
// Packed row width: MSB-first (module x=0 -> bit 7).
constexpr int kRowBytes = 3;
constexpr int kModules = kSize * kSize;
constexpr int kDataCodewords = 19;
constexpr int kEcCodewords = 7;
constexpr int kTotalCodewords = kDataCodewords + kEcCodewords;

namespace detail {

// GF(2^8/0x11D) multiply, no lookup tables.
inline uint8_t rsMul(uint8_t x, uint8_t y) {
  uint16_t z = 0;
  for (int i = 7; i >= 0; --i) {
    z = static_cast<uint16_t>((z << 1) ^ ((z >> 7) * 0x11Du));
    z ^= static_cast<uint16_t>(((y >> i) & 1u) * x);
  }
  return static_cast<uint8_t>(z);
}

inline bool getBit(uint16_t x, int i) { return ((x >> i) & 1u) != 0; }

inline void setModule(uint8_t* modules, bool* isFunc, int x, int y, bool dark) {
  modules[y * kSize + x] = dark ? 1u : 0u;
  isFunc[y * kSize + x] = true;
}

// 9x9 finder block (7x7 pattern + white separator ring) centred at (cx, cy).
inline void drawFinder(uint8_t* modules, bool* isFunc, int cx, int cy) {
  for (int dy = -4; dy <= 4; ++dy) {
    for (int dx = -4; dx <= 4; ++dx) {
      const int xx = cx + dx;
      const int yy = cy + dy;
      if (xx < 0 || xx >= kSize || yy < 0 || yy >= kSize) {
        continue;
      }
      int dist = dx < 0 ? -dx : dx;
      const int ady = dy < 0 ? -dy : dy;
      if (ady > dist) {
        dist = ady;
      }
      setModule(modules, isFunc, xx, yy, dist != 2 && dist != 4);
    }
  }
}

}  // namespace detail

// 6 ASCII digits -> 21x21 bitmap (0 = light, 1 = dark, row-major).
inline bool encode6Digits(const uint8_t code6[6], uint8_t modules[kModules]) {
  if (code6 == nullptr || modules == nullptr) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    if (code6[i] < '0' || code6[i] > '9') {
      return false;
    }
  }

  // ── Data codewords (numeric mode) ───────────────────────────────────
  uint8_t data[kDataCodewords];
  for (int i = 0; i < kDataCodewords; ++i) {
    data[i] = 0;
  }
  int bitLen = 0;
  auto appendBits = [&](uint16_t val, int n) {
    for (int i = n - 1; i >= 0; --i) {
      if (detail::getBit(val, i)) {
        data[bitLen >> 3] |= static_cast<uint8_t>(1u << (7 - (bitLen & 7)));
      }
      ++bitLen;
    }
  };
  appendBits(0x1, 4);  // Numeric mode indicator 0001.
  appendBits(6, 10);   // Character count (V1-9 numeric width is 10 bits).
  for (int g = 0; g < 2; ++g) {
    const int d0 = code6[g * 3] - '0';
    const int d1 = code6[g * 3 + 1] - '0';
    const int d2 = code6[g * 3 + 2] - '0';
    appendBits(static_cast<uint16_t>(d0 * 100 + d1 * 10 + d2), 10);
  }
  const int capacityBits = kDataCodewords * 8;
  int term = capacityBits - bitLen;
  if (term > 4) {
    term = 4;
  }
  appendBits(0, term);           // Terminator (up to 4 zero bits).
  appendBits(0, -bitLen & 7);    // Pad to the next byte boundary.
  const int used = bitLen >> 3;  // 5 bytes at this point.
  for (int i = used; i < kDataCodewords; ++i) {
    data[i] = ((i - used) % 2 == 0) ? 0xEC : 0x11;  // Standard pad bytes.
  }

  // ── Reed-Solomon EC (degree 7, single block) ────────────────────────
  uint8_t divisor[kEcCodewords];  // Generator poly sans leading term.
  for (int i = 0; i < kEcCodewords; ++i) {
    divisor[i] = 0;
  }
  divisor[kEcCodewords - 1] = 1;
  uint8_t root = 1;
  for (int i = 0; i < kEcCodewords; ++i) {
    for (int j = 0; j < kEcCodewords; ++j) {
      divisor[j] = detail::rsMul(divisor[j], root);
      if (j + 1 < kEcCodewords) {
        divisor[j] ^= divisor[j + 1];
      }
    }
    root = detail::rsMul(root, 0x02);
  }
  uint8_t ec[kEcCodewords];
  for (int i = 0; i < kEcCodewords; ++i) {
    ec[i] = 0;
  }
  for (int i = 0; i < kDataCodewords; ++i) {
    const uint8_t factor = static_cast<uint8_t>(data[i] ^ ec[0]);
    for (int j = 0; j < kEcCodewords - 1; ++j) {
      ec[j] = ec[j + 1];
    }
    ec[kEcCodewords - 1] = 0;
    for (int j = 0; j < kEcCodewords; ++j) {
      ec[j] ^= detail::rsMul(divisor[j], factor);
    }
  }
  uint8_t all[kTotalCodewords];
  for (int i = 0; i < kDataCodewords; ++i) {
    all[i] = data[i];
  }
  for (int i = 0; i < kEcCodewords; ++i) {
    all[kDataCodewords + i] = ec[i];
  }

  // ── Matrix: function patterns ───────────────────────────────────────
  bool isFunc[kModules];
  for (int i = 0; i < kModules; ++i) {
    modules[i] = 0;
    isFunc[i] = false;
  }
  for (int i = 0; i < kSize; ++i) {  // Timing row 6 / column 6 …
    detail::setModule(modules, isFunc, i, 6, (i % 2) == 0);
    detail::setModule(modules, isFunc, 6, i, (i % 2) == 0);
  }
  // …overwritten by the finders where they overlap.
  detail::drawFinder(modules, isFunc, 3, 3);
  detail::drawFinder(modules, isFunc, kSize - 4, 3);
  detail::drawFinder(modules, isFunc, 3, kSize - 4);

  // Format bits for ECC-L (01) + mask 0: data=01000, BCH(15,5) remainder, XOR mask 101010000010010.
  const uint16_t fmtData = 0x08;  // 01 (L) || 000 (mask 0).
  uint16_t rem = fmtData;
  for (int i = 0; i < 10; ++i) {
    rem = static_cast<uint16_t>((rem << 1) ^ (((rem >> 9) & 1u) * 0x537u));
  }
  const uint16_t fmtBits = static_cast<uint16_t>((fmtData << 10 | rem) ^ 0x5412u);
  for (int i = 0; i <= 5; ++i) {
    detail::setModule(modules, isFunc, 8, i, detail::getBit(fmtBits, i));
  }
  detail::setModule(modules, isFunc, 8, 7, detail::getBit(fmtBits, 6));
  detail::setModule(modules, isFunc, 8, 8, detail::getBit(fmtBits, 7));
  detail::setModule(modules, isFunc, 7, 8, detail::getBit(fmtBits, 8));
  for (int i = 9; i <= 14; ++i) {
    detail::setModule(modules, isFunc, 14 - i, 8, detail::getBit(fmtBits, i));
  }
  for (int i = 0; i <= 7; ++i) {
    detail::setModule(modules, isFunc, kSize - 1 - i, 8, detail::getBit(fmtBits, i));
  }
  for (int i = 8; i <= 14; ++i) {
    detail::setModule(modules, isFunc, 8, kSize - 15 + i, detail::getBit(fmtBits, i));
  }
  detail::setModule(modules, isFunc, 8, kSize - 8, true);  // Dark module.

  // ── Codeword zigzag (V1 has no remainder bits) ─
  int bit = 0;
  const int totalBits = kTotalCodewords * 8;
  for (int right = kSize - 1; right >= 1; right -= 2) {
    int r = right;
    if (r <= 6) {
      r -= 1;  // Skip the vertical timing column.
    }
    for (int vert = 0; vert < kSize; ++vert) {
      for (int j = 0; j < 2; ++j) {
        const int x = r - j;
        const bool upward = ((r + 1) & 2) == 0;
        const int y = upward ? (kSize - 1 - vert) : vert;
        if (!isFunc[y * kSize + x] && bit < totalBits) {
          modules[y * kSize + x] = detail::getBit(all[bit >> 3], 7 - (bit & 7)) ? 1u : 0u;
          ++bit;
        }
      }
    }
  }

  // ── Fixed mask 0: (x + y) % 2 == 0 flips data modules only ─
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      if (!isFunc[y * kSize + x] && ((x + y) % 2 == 0)) {
        modules[y * kSize + x] ^= 1u;
      }
    }
  }
  return true;
}

inline uint8_t getModule(const uint8_t modules[kModules], int x, int y) {
  if (modules == nullptr || x < 0 || x >= kSize || y < 0 || y >= kSize) {
    return 0;
  }
  return static_cast<uint8_t>(modules[y * kSize + x] & 1u);
}

// Row y -> 3 bytes MSB-first; unused low bits of byte 2 are zero.
inline void rowBytes(const uint8_t modules[kModules], int y, uint8_t out[kRowBytes]) {
  if (modules == nullptr || out == nullptr || y < 0 || y >= kSize) {
    return;
  }
  for (int i = 0; i < kRowBytes; ++i) {
    out[i] = 0;
  }
  for (int x = 0; x < kSize; ++x) {
    if (modules[y * kSize + x] & 1u) {
      out[x >> 3] |= static_cast<uint8_t>(1u << (7 - (x & 7)));
    }
  }
}

constexpr int kPackedLen = kSize * kRowBytes;

inline void packBitmap(const uint8_t modules[kModules], uint8_t out[kPackedLen]) {
  if (modules == nullptr || out == nullptr) {
    return;
  }
  for (int y = 0; y < kSize; ++y) {
    rowBytes(modules, y, out + y * kRowBytes);
  }
}

}  // namespace qr
