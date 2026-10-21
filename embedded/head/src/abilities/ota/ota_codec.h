#pragma once
// desky-head OTA ability — pure update grammar (Arduino-free, host-tested).
//
// Ability layer: ota_codec (pure serialization/validation, 100% native-testable).
// Moved verbatim from middleware/ota_client.h (Task 5 split): the ESP image
// magic + URL grammar, version pack/compare, magic/slot/stream gates, the
// manifest (SHA256 hex) compare, and the IDLE -> FETCH -> VERIFY -> COMMIT
// lifecycle. No ARDUINO, no sockets — host Unity tests pin every rule here.
//
// Integrity story (unchanged): plain HTTP only, manifest is the optional
// X-Firmware-Sha256 response header over the exact image bytes, plus the
// 0xE9 image-magic + Content-Length-vs-slot checks — a torn or tampered
// stream never commits.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "link/ota_manifest.h"

namespace ota {

// ESP32 boot image magic (first byte of a valid app image). Tested as the
// literal 0xE9 on both sides of the seam.
constexpr uint8_t kImageMagic = 0xE9;

// SHA256 manifest form: exactly 64 hex chars, no 0x prefix, no whitespace.
// Compared case-insensitively (servers may emit upper or lower case).
constexpr size_t kShaHexLen = 64;

// Plain HTTP only (see ota_task.h header note). Default port when the URL omits one.
constexpr uint16_t kDefaultHttpPort = 80;

// Fixed caps (no heap anywhere in the path; overlong URLs fail, never truncate).
constexpr size_t kHostCap = 64;   // incl NUL
constexpr size_t kPathCap = 128;  // incl NUL

struct Url {
  char host[kHostCap];
  uint16_t port;
  char path[kPathCap];
};

// Parse "http://host[:port]/path". False on: null/empty input, any scheme
// other than lowercase "http://" (rejects https/ftp/bare hosts), empty host,
// whitespace anywhere, '@' anywhere in the authority (credentials fail
// closed — never stripped, never logged), IPv6 literals (unsupported,
// documented), empty/non-numeric/out-of-range port, host or path exceeding
// the fixed caps. A missing path defaults to "/".
inline bool parseUrl(const char* url, Url& out) {
  out.host[0] = '\0';
  out.port = kDefaultHttpPort;
  out.path[0] = '\0';
  if (url == nullptr) {
    return false;
  }
  const char* kScheme = "http://";
  size_t i = 0;
  for (; kScheme[i] != '\0'; ++i) {
    if (url[i] != kScheme[i]) {
      return false;
    }
  }
  const char* auth = url + i;
  const char* slash = nullptr;
  for (const char* p = auth; *p != '\0'; ++p) {
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
      return false;
    }
    if (*p == '/' && slash == nullptr) {
      slash = p;
    }
  }
  const char* authEnd = (slash != nullptr) ? slash : auth + strlen(auth);
  if (authEnd == auth) {
    return false;  // empty host
  }
  for (const char* p = auth; p < authEnd; ++p) {
    if (*p == '@' || *p == '[' || *p == ']') {
      return false;
    }
  }
  const char* colon = nullptr;
  for (const char* p = auth; p < authEnd; ++p) {
    if (*p == ':') {
      if (colon != nullptr) {
        return false;  // second colon: no IPv6, no userinfo — reject
      }
      colon = p;
    }
  }
  const char* hostEnd = (colon != nullptr) ? colon : authEnd;
  const size_t hostLen = static_cast<size_t>(hostEnd - auth);
  if (hostLen == 0 || hostLen >= kHostCap) {
    return false;
  }
  if (colon != nullptr) {
    const char* ps = colon + 1;
    if (ps == authEnd) {
      return false;  // empty port
    }
    uint32_t port = 0;
    for (const char* p = ps; p < authEnd; ++p) {
      if (*p < '0' || *p > '9') {
        return false;
      }
      port = port * 10u + static_cast<uint32_t>(*p - '0');
      if (port > 65535u) {
        return false;
      }
    }
    if (port == 0) {
      return false;
    }
    out.port = static_cast<uint16_t>(port);
  }
  memcpy(out.host, auth, hostLen);
  out.host[hostLen] = '\0';
  if (slash == nullptr) {
    out.path[0] = '/';
    out.path[1] = '\0';
  } else {
    const size_t pathLen = strlen(slash);
    if (pathLen == 0 || pathLen >= kPathCap) {
      return false;
    }
    memcpy(out.path, slash, pathLen + 1);
  }
  return true;
}

// Monotonic uint32 version. packVersion lays major/minor/patch at bits
// 31..24 / 23..16 / 15..0 so plain numeric order == semver order for packed
// values; raw build counters compare numerically too. compareVersion returns
// -1 when a is older, 0 when equal, +1 when a is newer. (Phase 1 carries no
// version source; the gate wires to the X-Firmware-Version header in Phase 2.)
inline uint32_t packVersion(uint8_t major, uint8_t minor, uint16_t patch) {
  return (static_cast<uint32_t>(major) << 24) | (static_cast<uint32_t>(minor) << 16) | patch;
}

inline int compareVersion(uint32_t a, uint32_t b) {
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
}

// First byte of the stream must be the ESP image magic (0xE9). Checked on
// the first received chunk before any further write.
inline bool hasOtaMagic(const uint8_t* buf, size_t len) {
  if (buf == nullptr || len == 0) {
    return false;
  }
  return buf[0] == kImageMagic;
}

// Image must be non-empty and fit the running partition table's app slot
// (slot bytes are a parameter so host tests pin the policy, not the board).
inline bool fitsSlot(size_t imageBytes, uint32_t slotBytes) {
  if (imageBytes == 0 || slotBytes == 0) {
    return false;
  }
  return imageBytes <= static_cast<size_t>(slotBytes);
}

// Stream completeness gate: a short stream is torn, never committed.
inline bool streamComplete(uint32_t received, uint32_t total) {
  if (total == 0) {
    return false;
  }
  return received >= total;
}

inline bool isHexChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Exactly 64 hex chars + NUL (no prefix, no whitespace, no truncation).
inline bool isHex64(const char* s) {
  if (s == nullptr) {
    return false;
  }
  for (size_t k = 0; k < kShaHexLen; ++k) {
    if (!isHexChar(s[k])) {
      return false;
    }
  }
  return s[kShaHexLen] == '\0';
}

inline char lowerHex(char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c + 32) : c; }

// Manifest compare: both sides must be valid 64-hex strings; compare is
// case-insensitive. False on any malformed input (fail closed).
inline bool manifestMatches(const char* actualHex, const char* expectedHex) {
  if (!isHex64(actualHex) || !isHex64(expectedHex)) {
    return false;
  }
  for (size_t k = 0; k < kShaHexLen; ++k) {
    if (lowerHex(actualHex[k]) != lowerHex(expectedHex[k])) {
      return false;
    }
  }
  return true;
}

// 32-byte SHA256 digest -> 64 lowercase hex chars + NUL (pure, host-tested;
// the firmware half feeds it the mbedTLS digest at VERIFY time).
inline void bytesToHex64(const uint8_t* digest, char* out) {
  static const char kHex[] = "0123456789abcdef";
  if (digest == nullptr || out == nullptr) {
    return;
  }
  for (size_t k = 0; k < 32; ++k) {
    out[2 * k] = kHex[(digest[k] >> 4) & 0xF];
    out[2 * k + 1] = kHex[digest[k] & 0xF];
  }
  out[64] = '\0';
}

// Update lifecycle: IDLE -> FETCH -> VERIFY -> COMMIT -> IDLE. CHUNK keeps
// FETCH (per-write progress); any ABORT returns to IDLE (slot untouched).
// Anything else is rejected (false, no state change) — the firmware half
// treats a rejected event as an internal error and aborts the download.
enum class State : uint8_t { kIdle, kFetch, kVerify, kCommit };
enum class Event : uint8_t { kStart, kChunk, kStreamEnd, kVerifyOk, kVerifyFail, kCommitOk, kCommitFail, kAbort };

inline const char* stateName(State s) {
  switch (s) {
    case State::kIdle:
      return "IDLE";
    case State::kFetch:
      return "FETCH";
    case State::kVerify:
      return "VERIFY";
    case State::kCommit:
      return "COMMIT";
  }
  return "UNKNOWN";
}

inline bool step(State& s, Event e) {
  switch (s) {
    case State::kIdle:
      if (e == Event::kStart) {
        s = State::kFetch;
        return true;
      }
      return e == Event::kAbort;  // abort-while-idle is a no-op success
    case State::kFetch:
      if (e == Event::kChunk) {
        return true;
      }
      if (e == Event::kStreamEnd) {
        s = State::kVerify;
        return true;
      }
      if (e == Event::kAbort) {
        s = State::kIdle;
        return true;
      }
      return false;
    case State::kVerify:
      if (e == Event::kVerifyOk) {
        s = State::kCommit;
        return true;
      }
      if (e == Event::kVerifyFail || e == Event::kAbort) {
        s = State::kIdle;
        return true;
      }
      return false;
    case State::kCommit:
      if (e == Event::kCommitOk || e == Event::kCommitFail || e == Event::kAbort) {
        s = State::kIdle;
        return true;
      }
      return false;
  }
  return false;
}

}  // namespace ota
