#pragma once
// Carrier OTA manifest + URL vocabulary — shared, header-only, Arduino-free.
//
// Cross-seam contract (core stages/serves, head pulls/compares): the OTA URL
// grammar, the manifest text format, and the pull-gate rule live ONCE here.
// Both firmwares and both host suites call these — never mirrored constants,
// never re-derived parsing. The length bound pins to the codec contract
// (link_codec.h kOtaUrlMin/MaxLen: host tests assert equality both sides).
//
// Manifest text (GET /head-version), strict lines, order-free:
//   version=<u32 decimal>
//   size=<u32 decimal, image bytes>
//   sha256=<64 lowercase-or-upper hex, no 0x>
// Exactly one of each; extra lines are ignored. CRLF tolerated.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace otamanifest {

// URL bound: pinned equal to link::kOtaUrlMin/MaxLen by host tests both ends.
constexpr size_t kUrlMax = 128;

// Manifest caps: three short lines fit with margin; overlong bodies refuse.
constexpr size_t kShaHexLen = 64;
constexpr size_t kManifestCap = 256;

// Sibling manifest for a staged binary URL: only URLs ending in "/head.bin"
// have a manifest (replace the last segment with "head-version"). False
// otherwise — the caller pulls the URL directly (no version gate).
constexpr char kBinSuffix[] = "/head.bin";
constexpr char kManifestName[] = "head-version";

struct Manifest {
  uint32_t version = 0;
  uint32_t size = 0;
  char sha[kShaHexLen + 1] = {};
};

inline bool isHexChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// OTA URL bytes: 1..kUrlMax printable ASCII, no '@' (credentials never ride
// or log), no whitespace (grammar enforced fully by ota::parseUrl at start).
inline bool isValidOtaUrlBytes(const uint8_t* payload, size_t len) {
  if (payload == nullptr || len == 0 || len > kUrlMax) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const uint8_t b = payload[i];
    if (b < 0x20u || b > 0x7Eu) {
      return false;
    }
    if (b == '@' || b == ' ' || b == '\t' || b == '\r' || b == '\n') {
      return false;
    }
  }
  return true;
}

// Validated copy into a caller-owned 129B buffer (+NUL). False on any
// grammar violation (fail-closed); `out` untouched on failure.
inline bool copyOtaUrl(const uint8_t* payload, uint16_t len, char* out129) {
  if (out129 == nullptr || !isValidOtaUrlBytes(payload, len)) {
    return false;
  }
  for (uint16_t i = 0; i < len; ++i) {
    out129[i] = static_cast<char>(payload[i]);
  }
  out129[len] = '\0';
  return true;
}

// Strict u32 decimal (non-empty, digits only, fits u32). False otherwise.
inline bool parseU32(const char* s, uint32_t& out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  uint64_t v = 0;
  for (const char* p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    v = v * 10u + static_cast<uint64_t>(*p - '0');
    if (v > 0xFFFFFFFFu) {
      return false;
    }
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// Exactly 64 hex chars (case-insensitive) + NUL.
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

// Build the manifest text. False on null/short buffer or bad sha (the
// version/size are plain u32; sha must be valid 64-hex).
inline bool buildManifest(uint32_t version, uint32_t size, const char* shaHex, char* out, size_t cap) {
  if (out == nullptr || cap == 0 || !isHex64(shaHex)) {
    return false;
  }
  // snprintf-style manual build (no stdio dependency in shared headers):
  // "version=%u\nsize=%u\nsha256=%s\n". Compose via digit emission.
  char tmp[2][16];
  uint32_t vals[2] = {version, size};
  size_t lens[2] = {0, 0};
  for (int v = 0; v < 2; ++v) {
    char rev[16];
    size_t rl = 0;
    uint32_t x = vals[v];
    do {
      rev[rl++] = static_cast<char>('0' + (x % 10u));
      x /= 10u;
    } while (x > 0 && rl < sizeof(rev));
    for (size_t i = 0; i < rl; ++i) {
      tmp[v][i] = rev[rl - 1 - i];
    }
    tmp[v][rl] = '\0';
    lens[v] = rl;
  }
  const char* kP0 = "version=";
  const char* kP1 = "\nsize=";
  const char* kP2 = "\nsha256=";
  const size_t need = strlen(kP0) + lens[0] + strlen(kP1) + lens[1] + strlen(kP2) + kShaHexLen + 1 /*\n*/ + 1 /*NUL*/;
  if (need > cap) {
    return false;
  }
  char* p = out;
  for (const char* s = kP0; *s != '\0'; ++s) {
    *p++ = *s;
  }
  for (size_t i = 0; i < lens[0]; ++i) {
    *p++ = tmp[0][i];
  }
  for (const char* s = kP1; *s != '\0'; ++s) {
    *p++ = *s;
  }
  for (size_t i = 0; i < lens[1]; ++i) {
    *p++ = tmp[1][i];
  }
  for (const char* s = kP2; *s != '\0'; ++s) {
    *p++ = *s;
  }
  for (size_t i = 0; i < kShaHexLen; ++i) {
    *p++ = shaHex[i];
  }
  *p++ = '\n';
  *p = '\0';
  return true;
}

// Parse manifest text (NUL-terminated, <= kManifestCap caller's duty).
// Exactly one version=, one size=, one sha256= line; duplicates, missing, or
// malformed values fail closed.
inline bool parseManifest(const char* text, Manifest& out) {
  out.version = 0;
  out.size = 0;
  out.sha[0] = '\0';
  if (text == nullptr) {
    return false;
  }
  int nVersion = 0;
  int nSize = 0;
  int nSha = 0;
  const char* p = text;
  while (*p != '\0') {
    // One line: [start, end).
    const char* start = p;
    while (*p != '\0' && *p != '\n') {
      ++p;
    }
    size_t len = static_cast<size_t>(p - start);
    if (*p == '\n') {
      ++p;
    }
    if (len > 0 && start[len - 1] == '\r') {
      --len;
    }
    if (len == 0) {
      continue;  // blank lines ignored
    }
    char line[128];
    if (len >= sizeof(line)) {
      return false;
    }
    for (size_t i = 0; i < len; ++i) {
      line[i] = start[i];
    }
    line[len] = '\0';
    if (strncmp(line, "version=", 8) == 0) {
      if (!parseU32(line + 8, out.version)) {
        return false;
      }
      ++nVersion;
    } else if (strncmp(line, "size=", 5) == 0) {
      if (!parseU32(line + 5, out.size)) {
        return false;
      }
      ++nSize;
    } else if (strncmp(line, "sha256=", 7) == 0) {
      if (!isHex64(line + 7)) {
        return false;
      }
      memcpy(out.sha, line + 7, kShaHexLen + 1);
      ++nSha;
    }
    // Unknown lines ignored (forward-compatible).
  }
  return nVersion == 1 && nSize == 1 && nSha == 1;
}

// Manifest URL for a staged binary URL: replace the last path segment with
// "head-version", only when the URL ends in "/head.bin". Bounded copy into a
// caller-owned 129B buffer (+NUL). False otherwise (caller pulls directly).
inline bool manifestUrlFor(const char* binUrl, char* out129) {
  if (binUrl == nullptr || out129 == nullptr) {
    return false;
  }
  const size_t urlLen = strlen(binUrl);
  const size_t sufLen = strlen(kBinSuffix);
  if (urlLen <= sufLen || urlLen > kUrlMax) {
    return false;
  }
  if (strcmp(binUrl + urlLen - sufLen, kBinSuffix) != 0) {
    return false;
  }
  const size_t prefix = urlLen - sufLen + 1;  // keep the '/' before head.bin
  const size_t manLen = strlen(kManifestName);
  if (prefix + manLen + 1 > kUrlMax + 1) {
    return false;
  }
  memcpy(out129, binUrl, prefix);
  memcpy(out129 + prefix, kManifestName, manLen + 1);
  return true;
}

// Pull gate: pull only when the manifest version is NEWER than the running
// image. Numeric order == version order for monotonic u32 versions (same rule
// as ota::compareVersion; host tests assert equivalence on samples).
inline bool shouldPull(uint32_t manifestVersion, uint32_t currentVersion) { return manifestVersion > currentVersion; }

// POST ?version= decision (single source of truth: the version rides the
// upload, never a second channel). Atomic with the bytes by construction:
// - no version param: keep current (re-POSTing a binary never resets its label).
// - valid decimal u32: adopt it together with the bytes at commit.
// - malformed: reject (caller preserves previous bytes + version untouched).
enum class VersionApply : uint8_t { kKeep, kAdopt, kReject };

struct VersionDecision {
  VersionApply action = VersionApply::kKeep;
  uint32_t version = 0;
};

inline VersionDecision decideUploadVersion(bool present, bool valid, uint32_t value, uint32_t current) {
  VersionDecision r;
  r.version = current;
  if (!present) {
    r.action = VersionApply::kKeep;
    return r;
  }
  if (!valid) {
    r.action = VersionApply::kReject;
    return r;
  }
  r.action = VersionApply::kAdopt;
  r.version = value;
  return r;
}

}  // namespace otamanifest
