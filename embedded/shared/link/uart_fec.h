#pragma once
// UART POC Reed-Solomon-style FEC core — Arduino-free (stdint/stddef only),
// header-only, namespace uartpoc::fec.
//
// Systematic Vandermonde erasure code over GF(2^8) (poly 0x11D):
//   parity p_j[b] = sum_i V[j][i] * d_i[b],  V[j][i] = (i+1)^(j+1)
// (1-based data index, j = 0-based parity row). Row 0 is a plain XOR.
//
// Guarantees: no heap, no Arduino, no String, no static state. encode() is a
// pure function of its inputs; recover() solves into caller buffers using only
// caller-provided scratch. Stride padding is owned here: encode() zero-pads
// any short block (lens[i] < stride) internally, and recover() always emits
// full-stride blocks — the caller truncates block N-1 to last_len.
//
// Flags: parity CHUNKs set flag bit 2 (kFlagParity = 0x04) next to
// FLAG_LAST_CHUNK / FLAG_SYNTHETIC in uart_frame.h. Default K=2 is a
// call-site concern; this core supports up to kMaxParity = 4.

#include <stddef.h>
#include <stdint.h>

namespace uartpoc {
namespace fec {

constexpr uint8_t kFlagParity = 0x04;
constexpr uint8_t kMaxParity = 4;
constexpr uint8_t kMaxData = 64;
constexpr size_t kMaxStride = 1024;  // Mirrors uart_frame.h kMaxPayload (chunk ceiling).
constexpr size_t kRecoverScratchMin = 32;

namespace detail {

// Branch-free shift-and-reduce multiply (reduction constant 0x1D = low byte of
// 0x11D). Constexpr so the log/exp tables below build at compile time.
constexpr uint8_t gfMulSlow(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if ((b & 1U) != 0U) {
      p = static_cast<uint8_t>(p ^ a);
    }
    const bool hi = (a & 0x80U) != 0U;
    a = static_cast<uint8_t>(a << 1);
    if (hi) {
      a = static_cast<uint8_t>(a ^ 0x1DU);
    }
    b = static_cast<uint8_t>(b >> 1);
  }
  return p;
}

struct Tables {
  uint8_t exp[512];
  uint8_t log[256];
};

// Generator 2 is primitive mod 0x11D (order 255 — verified by host test via
// static_asserts below: 85/51/15 are 255/3, 255/5, 255/17).
constexpr Tables buildTables() {
  Tables t = {};
  uint8_t x = 1;
  for (uint16_t i = 0; i < 255; ++i) {
    t.exp[i] = x;
    t.log[x] = static_cast<uint8_t>(i);
    x = gfMulSlow(x, 2);
  }
  for (uint16_t i = 255; i < 512; ++i) {
    t.exp[i] = t.exp[i - 255];
  }
  return t;
}

}  // namespace detail

constexpr detail::Tables kTables = detail::buildTables();
static_assert(kTables.exp[255] == 1, "fec: generator 2 must wrap at 255");
static_assert(kTables.exp[85] != 1 && kTables.exp[51] != 1 && kTables.exp[15] != 1,
              "fec: generator 2 must be primitive mod 0x11D");
static_assert(kTables.log[1] == 0 && kTables.log[2] == 1, "fec: log table sanity");

inline uint8_t gfMul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  return kTables.exp[static_cast<uint16_t>(kTables.log[a]) + kTables.log[b]];
}

inline uint8_t gfInv(uint8_t a) {
  if (a == 0) {
    return 0;
  }
  return kTables.exp[255 - kTables.log[a]];
}

inline uint8_t gfPow(uint8_t a, uint8_t e) {
  if (e == 0) {
    return 1;
  }
  if (a == 0) {
    return 0;
  }
  return kTables.exp[(static_cast<uint16_t>(kTables.log[a]) * e) % 255];
}

// Vandermonde coefficient: parity row j (0-based), data index i (0-based,
// 1-based on the wire: V = (i+1)^(j+1)).
inline uint8_t vandermonde(uint8_t row, uint8_t idx) {
  return gfPow(static_cast<uint8_t>(idx + 1), static_cast<uint8_t>(row + 1));
}

// Systematic encode. data[0..n) are data blocks, lens[i] their true lengths
// (<= stride; short blocks are zero-padded internally); parity_out[0..k) are
// stride-byte outputs. False (outputs untouched) on any null pointer or bad
// n == 0 / k == 0 / k > kMaxParity / n > kMaxData / stride == 0 /
// stride > kMaxStride / lens[i] > stride.
inline bool encode(const uint8_t* const* data, uint8_t n, uint8_t k, size_t stride, const size_t* lens,
                   uint8_t* const* parity_out) {
  if (data == nullptr || lens == nullptr || parity_out == nullptr) {
    return false;
  }
  if (n == 0 || n > kMaxData || k == 0 || k > kMaxParity) {
    return false;
  }
  if (stride == 0 || stride > kMaxStride) {
    return false;
  }
  for (uint8_t i = 0; i < n; ++i) {
    if (data[i] == nullptr || lens[i] > stride) {
      return false;
    }
  }
  for (uint8_t j = 0; j < k; ++j) {
    if (parity_out[j] == nullptr) {
      return false;
    }
  }
  uint8_t coef[kMaxData];
  for (uint8_t j = 0; j < k; ++j) {
    for (uint8_t i = 0; i < n; ++i) {
      coef[i] = vandermonde(j, i);
    }
    uint8_t* p = parity_out[j];
    for (size_t b = 0; b < stride; ++b) {
      uint8_t acc = 0;
      for (uint8_t i = 0; i < n; ++i) {
        const uint8_t d = (b < lens[i]) ? data[i][b] : 0;
        acc = static_cast<uint8_t>(acc ^ gfMul(coef[i], d));
      }
      p[b] = acc;
    }
  }
  return true;
}

enum class Recover : uint8_t { OK, NEED_MORE, UNRECOVERABLE };

// Erasure recovery over known positions. missing[i] marks lost data blocks
// (their buffers must still be valid: solved bytes are written back
// full-stride); parity_ok[j] marks usable parity blocks. last_len is block
// N-1's true length (0 = full stride) — validated only, the caller truncates.
// scratch must hold >= kRecoverScratchMin bytes whenever solving is needed.
//   OK            nothing lost, or all losses solved and verified.
//   NEED_MORE     losses <= K but fewer good parity blocks than losses.
//   UNRECOVERABLE bad args, losses > K (provably impossible), singular
//                 system, or a parity check mismatch (corrupt input — never
//                 returned as recovered). K = 0 recovers nothing: OK when
//                 nothing is missing, else UNRECOVERABLE.
inline Recover recover(uint8_t* const* data_io, const bool* missing, uint8_t n, const uint8_t* const* parity,
                       const bool* parity_ok, uint8_t k, size_t stride, size_t last_len, uint8_t* scratch,
                       size_t scratch_cap) {
  if (data_io == nullptr || missing == nullptr || n == 0 || n > kMaxData || k > kMaxParity) {
    return Recover::UNRECOVERABLE;
  }
  if (stride == 0 || stride > kMaxStride || last_len > stride) {
    return Recover::UNRECOVERABLE;
  }
  for (uint8_t i = 0; i < n; ++i) {
    if (data_io[i] == nullptr) {
      return Recover::UNRECOVERABLE;
    }
  }
  if (k > 0) {
    if (parity == nullptr || parity_ok == nullptr) {
      return Recover::UNRECOVERABLE;
    }
    for (uint8_t j = 0; j < k; ++j) {
      if (parity_ok[j] && parity[j] == nullptr) {
        return Recover::UNRECOVERABLE;
      }
    }
  }
  uint8_t erased[kMaxData];
  uint8_t n_missing = 0;
  for (uint8_t i = 0; i < n; ++i) {
    if (missing[i]) {
      erased[n_missing++] = i;
    }
  }
  if (n_missing == 0) {
    return Recover::OK;
  }
  if (k == 0 || n_missing > k) {
    return Recover::UNRECOVERABLE;  // K = 0 passthrough / provably impossible.
  }
  uint8_t good[kMaxParity];
  uint8_t n_good = 0;
  for (uint8_t j = 0; j < k; ++j) {
    if (parity_ok[j]) {
      good[n_good++] = j;
    }
  }
  if (n_good < n_missing) {
    return Recover::NEED_MORE;
  }
  if (scratch == nullptr || scratch_cap < kRecoverScratchMin) {
    return Recover::UNRECOVERABLE;
  }
  const uint8_t t = n_missing;
  const uint8_t cols = static_cast<uint8_t>(t + 1);
  for (size_t b = 0; b < stride; ++b) {
    // Augmented system from the first t good parity rows.
    for (uint8_t r = 0; r < t; ++r) {
      const uint8_t row = good[r];
      uint8_t rhs = parity[row][b];
      for (uint8_t i = 0; i < n; ++i) {
        if (!missing[i]) {
          rhs = static_cast<uint8_t>(rhs ^ gfMul(vandermonde(row, i), data_io[i][b]));
        }
      }
      for (uint8_t c = 0; c < t; ++c) {
        scratch[r * cols + c] = vandermonde(row, erased[c]);
      }
      scratch[r * cols + t] = rhs;
    }
    // Gauss-Jordan with partial pivoting.
    for (uint8_t col = 0; col < t; ++col) {
      uint8_t piv = col;
      while (piv < t && scratch[piv * cols + col] == 0) {
        ++piv;
      }
      if (piv == t) {
        return Recover::UNRECOVERABLE;  // Singular — must not guess.
      }
      if (piv != col) {
        for (uint8_t c = col; c < cols; ++c) {
          const uint8_t tmp = scratch[col * cols + c];
          scratch[col * cols + c] = scratch[piv * cols + c];
          scratch[piv * cols + c] = tmp;
        }
      }
      const uint8_t inv = gfInv(scratch[col * cols + col]);
      for (uint8_t c = col; c < cols; ++c) {
        scratch[col * cols + c] = gfMul(scratch[col * cols + c], inv);
      }
      for (uint8_t r = 0; r < t; ++r) {
        if (r == col) {
          continue;
        }
        const uint8_t f = scratch[r * cols + col];
        if (f != 0) {
          for (uint8_t c = col; c < cols; ++c) {
            scratch[r * cols + c] = static_cast<uint8_t>(scratch[r * cols + c] ^ gfMul(f, scratch[col * cols + c]));
          }
        }
      }
    }
    // Verify against EVERY good row before touching caller buffers.
    for (uint8_t g = 0; g < n_good; ++g) {
      const uint8_t row = good[g];
      uint8_t check = parity[row][b];
      for (uint8_t i = 0; i < n; ++i) {
        uint8_t v;
        if (!missing[i]) {
          v = data_io[i][b];
        } else {
          uint8_t c = 0;
          while (erased[c] != i) {
            ++c;
          }
          v = scratch[c * cols + t];
        }
        check = static_cast<uint8_t>(check ^ gfMul(vandermonde(row, i), v));
      }
      if (check != 0) {
        return Recover::UNRECOVERABLE;  // Corrupt parity/data — never emit.
      }
    }
    for (uint8_t c = 0; c < t; ++c) {
      data_io[erased[c]][b] = scratch[c * cols + t];
    }
  }
  return Recover::OK;
}

// Head parity-emit shape (POC task 2) — pure policy, Arduino-free, so host
// Unity tests pin it without hardware. planEmit() decides per frame whether
// K parity chunks follow the N data chunks; parityChunkFlags/Idx() pin the
// parity header contract (flags exactly kFlagParity — never LAST_CHUNK, which
// stays on data chunk N-1; payloadLen is always the full stride because
// encode() zero-pads short tails internally).
constexpr size_t kEmitDataCap = 16384;  // Max N*stride staged for parity (INTERNAL sizing lives at the call site).

struct EmitPlan {
  uint8_t k = 0;      // Parity chunks to emit.
  bool emit = false;  // False -> skip; the wire stays byte-identical to K=0.
};

inline EmitPlan planEmit(uint8_t cfgK, uint32_t nChunks, uint16_t stride) {
  EmitPlan p;
  if (cfgK == 0 || nChunks == 0 || nChunks > kMaxData) {
    return p;
  }
  if (stride == 0 || stride > kMaxStride) {
    return p;
  }
  if (static_cast<uint64_t>(nChunks) * stride > kEmitDataCap) {
    return p;
  }
  p.k = (cfgK > kMaxParity) ? kMaxParity : cfgK;
  p.emit = true;
  return p;
}

// Parity chunk header contract: flags are exactly IS_PARITY (never
// LAST_CHUNK or SYNTHETIC), chunkIdx runs N..N+K-1, payloadLen is stride.
inline uint8_t parityChunkFlags() { return kFlagParity; }

inline uint16_t parityChunkIdx(uint32_t nChunks, uint8_t j) { return static_cast<uint16_t>(nChunks + j); }

}  // namespace fec
}  // namespace uartpoc
