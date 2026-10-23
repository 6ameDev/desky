#pragma once
// OTA HTTP pull pump binding (firmware only).
// Plain HTTP only: integrity is the manifest (X-Firmware-Sha256) + 0xE9
// image-magic + Content-Length-vs-slot checks; torn streams never commit.
// Never logs URL credentials (parseUrl rejects '@').

#ifdef ARDUINO
// ── Firmware: HTTP pull updater (WiFiClient + Update, short-lived) ──────────

#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "abilities/display/display_task.h"
#include "abilities/ota/ota_codec.h"
#include "abilities/ota/ota_manager.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "link/ota_manifest.h"

#ifndef CFG_OTA_SLOT_BYTES
#define CFG_OTA_SLOT_BYTES 0x1F0000  // matches partitions_ota_nospiffs.csv app slots
#endif
#ifndef CFG_OTA_TIMEOUT_MS
#define CFG_OTA_TIMEOUT_MS 15000
#endif

// SHA256 via Arduino/mbedTLS; guarded so toolchains without it still build.
#if __has_include(<mbedtls/sha256.h>)
#include <mbedtls/sha256.h>
#define OTA_HAVE_SHA256 1
#else
#define OTA_HAVE_SHA256 0
#endif

class OtaUpdater {
 public:
  OtaUpdater()
      : display_(nullptr),
        state_(ota::State::kIdle),
        total_(0),
        received_(0),
        lastRxMs_(0),
        lastPct_(0),
        manifestSet_(false) {
    manifest_[0] = '\0';
  }

  // Stored, never owned; null-safe. Updater drives the OLED bar through it.
  void attachDisplay(DisplayRenderer* display) { display_ = display; }

  // While active, loop() skips the camera grab (flash/cache contention).
  bool active() const { return state_ != ota::State::kIdle; }

  // Carrier entry: pulls only when the staged manifest says the image is newer.
  bool startIfNewer(const char* url) {
    if (!otapolicy::canStart(state_)) {
      LOG_W("OTA", "busy state=%s, ignored", ota::stateName(state_));
      return false;
    }
    char murl[otamanifest::kUrlMax + 1];
    if (!otamanifest::manifestUrlFor(url, murl)) {
      return start(url);
    }
    otamanifest::Manifest man;
    if (!fetchManifest(murl, man)) {
      LOG_W("OTA", "no manifest, refused");
      return false;
    }
    if (!otamanifest::shouldPull(man.version, static_cast<uint32_t>(HEAD_FW_VERSION))) {
      LOG_I("OTA", "up to date version=%lu size=%lu", static_cast<unsigned long>(man.version),
            static_cast<unsigned long>(man.size));
      return false;
    }
    LOG_I("OTA", "update version=%lu size=%lu", static_cast<unsigned long>(man.version),
          static_cast<unsigned long>(man.size));
    return start(url);
  }

  // Start an OTA from a plain-http URL. Blocks briefly; the body pumps in poll().
  bool start(const char* url) {
    if (!otapolicy::canStart(state_)) {
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
    if (!otapolicy::statusOk(line)) {
      LOG_W("OTA", "bad status (want HTTP/* 200)");
      fail();
      return false;
    }
    // Content-Length is required (chunked refused); X-Firmware-Sha256 is optional.
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
        if (otapolicy::manifestValueOk(v)) {
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
    switch (otapolicy::checkLength(length, static_cast<uint32_t>(CFG_OTA_SLOT_BYTES))) {
      case otapolicy::LengthVerdict::kMissing:
        LOG_W("OTA", "no content-length, refused");
        fail();
        return false;
      case otapolicy::LengthVerdict::kTooBig:
        LOG_W("OTA", "too big bytes=%u slot=%u", static_cast<unsigned>(length),
              static_cast<unsigned>(CFG_OTA_SLOT_BYTES));
        fail();
        return false;
      case otapolicy::LengthVerdict::kOk:
        break;
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
    otapolicy::applyEvent(state_, ota::Event::kStart);
    displayShow(0);
    LOG_I("OTA", "fetch bytes=%u manifest=%d", static_cast<unsigned>(total_), manifestSet_ ? 1 : 0);
    return true;
  }

  // Body pump: call every loop(). Counters only, never URL/payload bytes.
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
      otapolicy::applyEvent(state_, ota::Event::kChunk);
      uint32_t report = 0;
      if (otapolicy::progressDue(received_, total_, lastPct_, report)) {
        displayShow(static_cast<uint8_t>(report));
        LOG_I("OTA", "progress bytes=%u/%u pct=%u", static_cast<unsigned>(received_), static_cast<unsigned>(total_),
              static_cast<unsigned>(lastPct_));
      }
    }
    if (ota::streamComplete(received_, total_)) {
      finish();
      return;
    }
    if (otapolicy::isTorn(client_.available() > 0, client_.connected(), received_, total_)) {
      LOG_W("OTA", "torn stream bytes=%u/%u, discarded", static_cast<unsigned>(received_),
            static_cast<unsigned>(total_));
      fail();
      return;
    }
    if (otapolicy::isStalled(now, lastRxMs_, static_cast<uint32_t>(CFG_OTA_TIMEOUT_MS))) {
      LOG_W("OTA", "stall timeout bytes=%u/%u", static_cast<unsigned>(received_), static_cast<unsigned>(total_));
      fail();
    }
  }

 private:
  bool readLine(char* buf, size_t cap) { return readLineFrom(client_, buf, cap); }

  void finish() {
    otapolicy::applyEvent(state_, ota::Event::kStreamEnd);
    displayShow(100);
#if OTA_HAVE_SHA256
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_, digest);
    mbedtls_sha256_free(&sha_);
    char actual[ota::kShaHexLen + 1];
    ota::bytesToHex64(digest, actual);
    const ota::Event verdict = otapolicy::verifyOutcome(manifestSet_, actual, manifest_);
    if (verdict == ota::Event::kVerifyFail) {
      LOG_W("OTA", "manifest mismatch, discarded");
      Update.abort();
      otapolicy::applyEvent(state_, verdict);
      displayFail();
      client_.stop();
      return;
    }
#endif
    if (!manifestSet_) {
      LOG_I("OTA", "no manifest, size+magic only");
    }
    otapolicy::applyEvent(state_, ota::Event::kVerifyOk);
    displayShow(100);
    const ota::Event commit = otapolicy::commitOutcome(Update.end(true));
    otapolicy::applyEvent(state_, commit);
    if (commit == ota::Event::kCommitOk) {
      displayHide();
      LOG_I("OTA", "ok bytes=%u rebooting", static_cast<unsigned>(received_));
      client_.stop();
      delay(100);
      ESP.restart();
    } else {
      LOG_W("OTA", "commit fail");
      displayFail();  // alert face, timeout back to the stored face
      client_.stop();
    }
  }

  void fail() {
    Update.abort();
    otapolicy::applyEvent(state_, ota::Event::kAbort);
    displayFail();  // alert face, timeout back to the stored face
    client_.stop();
  }

  // Manifest fetch for the carrier pull-gate (fail-closed, never logs URL bytes).
  bool fetchManifest(const char* murl, otamanifest::Manifest& man) {
    man.version = 0;
    man.size = 0;
    man.sha[0] = '\0';
    if (murl == nullptr) {
      return false;
    }
    // Never fetch a URL parseUrl rejects.
    ota::Url parts;
    if (!ota::parseUrl(murl, parts)) {
      return false;
    }
    WiFiClient mc;
    mc.setTimeout(3000);
    if (mc.connect(parts.host, parts.port, 5000) != 1) {
      return false;
    }
    char req[256];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", parts.path, parts.host);
    mc.print(req);
    char line[192];
    if (!readLineFrom(mc, line, sizeof(line))) {
      mc.stop();
      return false;
    }
    if (!otapolicy::statusOk(line)) {
      mc.stop();
      return false;
    }
    uint32_t length = 0;
    while (readLineFrom(mc, line, sizeof(line))) {
      if (line[0] == '\0') {
        break;  // blank line: end of headers
      }
      if (strncmp(line, "Content-Length:", 15) == 0 || strncmp(line, "content-length:", 15) == 0) {
        length = static_cast<uint32_t>(strtoul(line + 15, nullptr, 10));
      }
    }
    if (!otapolicy::manifestLengthOk(length)) {
      mc.stop();
      return false;
    }
    char body[otamanifest::kManifestCap];
    size_t got = 0;
    const uint32_t t0 = millis();
    while (got < length) {
      while (mc.available() == 0) {
        if (!mc.connected() || (millis() - t0) >= 5000) {
          mc.stop();
          return false;
        }
        delay(1);
        FaultManager::watchdogFeed();
      }
      const int c = mc.read();
      if (c < 0) {
        continue;
      }
      body[got++] = static_cast<char>(c);
      FaultManager::watchdogFeed();
    }
    mc.stop();
    body[got] = '\0';
    return otamanifest::parseManifest(body, man);
  }

  bool readLineFrom(WiFiClient& c, char* buf, size_t cap) {
    size_t n = 0;
    const uint32_t t0 = millis();
    while (true) {
      while (c.available() == 0) {
        if ((millis() - t0) >= 5000) {
          return false;
        }
        delay(1);
        FaultManager::watchdogFeed();
      }
      const int ch = c.read();
      if (ch < 0) {
        continue;
      }
      if (ch == '\n') {
        break;
      }
      if (ch != '\r' && n + 1 < cap) {
        buf[n++] = static_cast<char>(ch);
      }
    }
    buf[n] = '\0';
    return true;
  }

  void displayShow(uint8_t pct) {
    if (display_ != nullptr) {
      display_->showOta(pct);
    }
  }

  void displayHide() {
    if (display_ != nullptr) {
      display_->hideOta();
    }
  }

  void displayFail() {
    if (display_ != nullptr) {
      display_->failOta();
    }
  }

  DisplayRenderer* display_;
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
  uint8_t chunk_[1460];
};

#endif  // ARDUINO
