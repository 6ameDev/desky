#pragma once
// desky-head OTA update client (Phase 1: HTTP pull, CLI `ota <url>` trigger).
//
// STANDING RULE — do NOT ship a head image without this client: once the
// single USB session ends the head has no wired door (the MB board occupies
// every pin; field units expose no USB), so an image without the OTA path is
// unflashable except by re-opening the unit. main.cpp references this rule
// where the updater is constructed.
//
// Transport: plain HTTP ONLY (no TLS). The head joins a closed AP it never
// selects (channel follows the core AP); there is no CA story on a 4MB
// classic ESP32, so TLS would be cert theater. Integrity comes from the
// manifest (optional X-Firmware-Sha256 response header, SHA256 over the
// exact image bytes) plus the 0xE9 image-magic + Content-Length-vs-slot
// checks — a torn or tampered stream never commits.
//
// Torn-download safety: Update writes the INACTIVE ota slot while the
// running image is untouched; only a full-length, magic-checked,
// manifest-matching (when a manifest was sent) stream reaches Update.end().
// Anything else (short stream, stall, bad magic, manifest mismatch, commit
// failure) calls Update.abort() and the next boot runs the old image.
// USB reflash from the erase-session procedure is the backstop.
//
// No-auto-rollback: the Arduino Update path has no rollback/valid-marking
// (a bad-but-bootable image stays booted). Mark-valid stub is future work —
// see the OTA section in embedded/docs/link-learnings.md.
//
// Frozen-wire note: this client adds no :3333/:3334 traffic and touches no
// HB/link timing — it opens its own short-lived TCP connection to the URL
// host and closes it. Never logs URL credentials: parseUrl() rejects any
// authority containing '@', and start() logs host + counters only.
//
// Structure mirrors wifi_link.h: the top half (namespace ota) is
// Arduino-free pure logic (stdint/string only) so host Unity tests cover it;
// the bottom half (class OtaUpdater) is firmware-only (#ifdef ARDUINO):
// WiFiClient pump + Update writes, camera grab suspended by the main-loop
// gate (OtaUpdater::active()), WDT fed per chunk, counters-only logs.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ota {

// ESP32 boot image magic (first byte of a valid app image). Tested as the
// literal 0xE9 on both sides of the seam.
constexpr uint8_t kImageMagic = 0xE9;

// SHA256 manifest form: exactly 64 hex chars, no 0x prefix, no whitespace.
// Compared case-insensitively (servers may emit upper or lower case).
constexpr size_t kShaHexLen = 64;

// Plain HTTP only (see header note). Default port when the URL omits one.
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

#ifdef ARDUINO
// ── Firmware: HTTP pull updater (WiFiClient + Update, short-lived) ──────────

#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"

// Fallbacks keep this header compiling if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_OTA_SLOT_BYTES
#define CFG_OTA_SLOT_BYTES 0x1F0000  // must match partitions_ota_nospiffs.csv app slots
#endif
#ifndef CFG_OTA_TIMEOUT_MS
#define CFG_OTA_TIMEOUT_MS 15000
#endif

// SHA256 comes from the already-linked Arduino/mbedTLS (no new lib_deps).
// Guarded so a toolchain without the header degrades to size+magic checks
// with a loud log instead of breaking the build.
#if __has_include(<mbedtls/sha256.h>)
#include <mbedtls/sha256.h>
#define OTA_HAVE_SHA256 1
#else
#define OTA_HAVE_SHA256 0
#endif

class OtaUpdater {
 public:
  OtaUpdater() : state_(ota::State::kIdle), total_(0), received_(0), lastRxMs_(0), lastPct_(0), manifestSet_(false) {
    manifest_[0] = '\0';
  }

  // Main-loop camera gate: while active, loop() skips CameraDriver::update()
  // (grab suspended — a JPEG grab mid-flash contends PSRAM/flash cache with
  // the single fb_count=1 buffer; see camera_driver.h). The sensor stays
  // powered (no deinit/re-init cycle); only the periodic grab is paused.
  bool active() const { return state_ != ota::State::kIdle; }

  // Start an OTA from a plain-http URL. Works standalone — requires nothing
  // (link up OR down; association is the STA torrent below, not the B-link),
  // but logs the link state alongside for triage. Never logs the URL beyond
  // host + counters (parseUrl rejects '@', so no credentials can arrive).
  // Blocks briefly for connect + headers (WDT fed); the body pumps in poll().
  bool start(const char* url) {
    if (active()) {
      LOG_W("OTA", "busy state=%s, ignored", ota::stateName(state_));
      return false;
    }
    ota::Url parts;
    if (url == nullptr || !ota::parseUrl(url, parts)) {
      LOG_W("OTA", "bad url (want http://host[:port]/path)");
      return false;
    }
    const bool linkUp = (WiFi.status() == WL_CONNECTED);
    LOG_I("OTA", "start host=%s port=%u pathlen=%u link=%s", parts.host, static_cast<unsigned>(parts.port),
          static_cast<unsigned>(strlen(parts.path)), linkUp ? "up" : "down");
    manifestSet_ = false;
    manifest_[0] = '\0';
    total_ = 0;
    received_ = 0;
    client_.setTimeout(3000);
    if (client_.connect(parts.host, parts.port, 5000) != 1) {
      LOG_W("OTA", "connect fail host=%s port=%u", parts.host, static_cast<unsigned>(parts.port));
      return false;
    }
    char req[256];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", parts.path, parts.host);
    client_.print(req);
    char line[192];
    if (!readLine(line, sizeof(line))) {
      LOG_W("OTA", "no status line");
      fail();
      return false;
    }
    if (strncmp(line, "HTTP/", 5) != 0 || strstr(line, " 200 ") == nullptr) {
      LOG_W("OTA", "bad status (want HTTP/* 200)");
      fail();
      return false;
    }
    // Headers: Content-Length is REQUIRED (Update needs the size up front;
    // chunked encoding is refused). X-Firmware-Sha256 is the optional
    // manifest (64 hex chars over the exact image bytes).
    uint32_t length = 0;
    while (readLine(line, sizeof(line))) {
      if (line[0] == '\0') {
        break;  // blank line: end of headers
      }
      if (strncmp(line, "Content-Length:", 15) == 0 || strncmp(line, "content-length:", 15) == 0) {
        length = static_cast<uint32_t>(strtoul(line + 15, nullptr, 10));
      } else if (strncmp(line, "X-Firmware-Sha256:", 18) == 0) {
        const char* v = line + 18;
        while (*v == ' ' || *v == '\t') {
          ++v;
        }
#if OTA_HAVE_SHA256
        if (ota::isHex64(v)) {
          memcpy(manifest_, v, ota::kShaHexLen + 1);
          manifestSet_ = true;
        } else {
          LOG_W("OTA", "bad manifest header, ignored");
        }
#else
        (void)v;
        LOG_W("OTA", "manifest sent but no sha256 in toolchain, size+magic only");
#endif
      }
    }
    if (length == 0) {
      LOG_W("OTA", "no content-length, refused");
      fail();
      return false;
    }
    if (!ota::fitsSlot(length, static_cast<uint32_t>(CFG_OTA_SLOT_BYTES))) {
      LOG_W("OTA", "too big bytes=%u slot=%u", static_cast<unsigned>(length),
            static_cast<unsigned>(CFG_OTA_SLOT_BYTES));
      fail();
      return false;
    }
    total_ = length;
    if (!Update.begin(total_)) {
      LOG_W("OTA", "begin fail bytes=%u", static_cast<unsigned>(total_));
      fail();
      return false;
    }
#if OTA_HAVE_SHA256
    mbedtls_sha256_init(&sha_);
    mbedtls_sha256_starts(&sha_, 0);
#endif
    lastRxMs_ = millis();
    lastPct_ = 0;
    received_ = 0;
    ota::step(state_, ota::Event::kStart);
    LOG_I("OTA", "fetch bytes=%u manifest=%d", static_cast<unsigned>(total_), manifestSet_ ? 1 : 0);
    return true;
  }

  // Body pump: call every loop(). Reads available bytes, writes Update,
  // feeds the WDT per chunk (a ~2MB pull spans seconds), logs counters only
  // (bytes + 10% progress steps — never URL bytes, never payload bytes).
  void poll() {
    if (state_ != ota::State::kFetch) {
      return;
    }
    const uint32_t now = millis();
    while (client_.available() > 0 && received_ < total_) {
      const int n = client_.read(chunk_, sizeof(chunk_));
      if (n <= 0) {
        break;
      }
      if (received_ == 0 && !ota::hasOtaMagic(chunk_, static_cast<size_t>(n))) {
        LOG_W("OTA", "bad magic (not an ESP image), discarded");
        fail();
        return;
      }
      if (Update.write(chunk_, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
        LOG_W("OTA", "write fail at=%u", static_cast<unsigned>(received_));
        fail();
        return;
      }
#if OTA_HAVE_SHA256
      mbedtls_sha256_update(&sha_, chunk_, static_cast<size_t>(n));
#endif
      received_ += static_cast<uint32_t>(n);
      lastRxMs_ = now;
      FaultManager::watchdogFeed();
      ota::step(state_, ota::Event::kChunk);
      const uint32_t pct = (received_ * 100u) / total_;
      if (pct >= lastPct_ + 10u) {
        lastPct_ = pct - (pct % 10u);
        LOG_I("OTA", "progress bytes=%u/%u pct=%u", static_cast<unsigned>(received_), static_cast<unsigned>(total_),
              static_cast<unsigned>(lastPct_));
      }
    }
    if (ota::streamComplete(received_, total_)) {
      finish();
      return;
    }
    if (client_.available() == 0 && !client_.connected()) {
      LOG_W("OTA", "torn stream bytes=%u/%u, discarded", static_cast<unsigned>(received_),
            static_cast<unsigned>(total_));
      fail();
      return;
    }
    if ((now - lastRxMs_) >= static_cast<uint32_t>(CFG_OTA_TIMEOUT_MS)) {
      LOG_W("OTA", "stall timeout bytes=%u/%u", static_cast<unsigned>(received_), static_cast<unsigned>(total_));
      fail();
    }
  }

 private:
  // Blocking line read (start() only, never the pump): '\n'-terminated,
  // '\r' stripped, capped, 5s deadline with WDT feeds.
  bool readLine(char* buf, size_t cap) {
    size_t n = 0;
    const uint32_t t0 = millis();
    while (true) {
      while (client_.available() == 0) {
        if ((millis() - t0) >= 5000) {
          return false;
        }
        delay(1);
        FaultManager::watchdogFeed();
      }
      const int c = client_.read();
      if (c < 0) {
        continue;
      }
      if (c == '\n') {
        break;
      }
      if (c != '\r' && n + 1 < cap) {
        buf[n++] = static_cast<char>(c);
      }
    }
    buf[n] = '\0';
    return true;
  }

  // VERIFY -> COMMIT: manifest (when sent) then Update.end(). Success
  // reboots into the new slot; anything else aborts (old image boots).
  void finish() {
    ota::step(state_, ota::Event::kStreamEnd);  // -> VERIFY
#if OTA_HAVE_SHA256
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_, digest);
    mbedtls_sha256_free(&sha_);
    char actual[ota::kShaHexLen + 1];
    ota::bytesToHex64(digest, actual);
    if (manifestSet_ && !ota::manifestMatches(actual, manifest_)) {
      LOG_W("OTA", "manifest mismatch, discarded");
      Update.abort();
      ota::step(state_, ota::Event::kVerifyFail);
      client_.stop();
      return;
    }
#endif
    if (!manifestSet_) {
      LOG_I("OTA", "no manifest, size+magic only");
    }
    ota::step(state_, ota::Event::kVerifyOk);  // -> COMMIT
    if (Update.end(true)) {
      ota::step(state_, ota::Event::kCommitOk);
      LOG_I("OTA", "ok bytes=%u rebooting", static_cast<unsigned>(received_));
      client_.stop();
      delay(100);
      ESP.restart();
    } else {
      LOG_W("OTA", "commit fail");
      ota::step(state_, ota::Event::kCommitFail);
      client_.stop();
    }
  }

  void fail() {
    Update.abort();
    ota::step(state_, ota::Event::kAbort);
    client_.stop();
  }

  ota::State state_;
  WiFiClient client_;
  uint32_t total_;
  uint32_t received_;
  uint32_t lastRxMs_;
  uint32_t lastPct_;
  bool manifestSet_;
  char manifest_[ota::kShaHexLen + 1];
#if OTA_HAVE_SHA256
  mbedtls_sha256_context sha_;
#endif
  // Static chunk buffer (member -> BSS, never task stack, never heap).
  uint8_t chunk_[1460];
};

#endif  // ARDUINO
