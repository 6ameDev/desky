#pragma once
// OTA ability codec: pure update grammar (Arduino-free, host-tested).
// Plain HTTP only; torn or tampered streams never commit.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "link/ota_manifest.h"

namespace ota {

// First byte of a valid app image.
constexpr uint8_t kImageMagic = 0xE9;

// Exactly 64 hex chars, compared case-insensitively.
constexpr size_t kShaHexLen = 64;

constexpr uint16_t kDefaultHttpPort = 80;

// Fixed caps, no heap; overlong URLs fail, never truncate.
constexpr size_t kHostCap = 64;   // incl NUL
constexpr size_t kPathCap = 128;  // incl NUL

struct Url {
  char host[kHostCap];
  uint16_t port;
  char path[kPathCap];
};

// Parse "http://host[:port]/path". Fail-closed on any malformed input.
// A missing path defaults to "/".
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

// Monotonic uint32 version; plain numeric order == semver order for packed values.
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

// First stream byte must be the image magic.
inline bool hasOtaMagic(const uint8_t* buf, size_t len) {
  if (buf == nullptr || len == 0) {
    return false;
  }
  return buf[0] == kImageMagic;
}

// Image must be non-empty and fit the app slot.
inline bool fitsSlot(size_t imageBytes, uint32_t slotBytes) {
  if (imageBytes == 0 || slotBytes == 0) {
    return false;
  }
  return imageBytes <= static_cast<size_t>(slotBytes);
}

// A short stream is torn, never committed.
inline bool streamComplete(uint32_t received, uint32_t total) {
  if (total == 0) {
    return false;
  }
  return received >= total;
}

inline bool isHexChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

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

// False on any malformed input (fail closed).
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

// 32-byte SHA256 digest -> 64 lowercase hex chars + NUL.
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

// IDLE -> FETCH -> VERIFY -> COMMIT -> IDLE. Anything else is rejected.
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
