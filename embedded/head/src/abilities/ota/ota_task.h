#pragma once
// desky-head OTA ability — HTTP pull pump binding (firmware only).
//
// Ability layer: ota_task (RTOS/event binder gluing the OTA manager to the
// WiFiClient/Update pump). Moved from middleware/ota_client.h (Task 5 split):
// class OtaUpdater keeps its exact public API and name (attachFace / active /
// startIfNewer / start / poll, plus the faceShow/faceHide/faceFail hooks and
// the fetchManifest/readLineFrom privates). Grammar + versions delegate to
// ota_codec.h, lifecycle policy (start-gate, header validation, 10% progress
// rule, VERIFY/COMMIT verdicts, torn/stall abort mapping) to ota_manager.h —
// the single ota::State still lives in the codec (applied via
// otapolicy::applyEvent, never forked).
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

#ifdef ARDUINO
// ── Firmware: HTTP pull updater (WiFiClient + Update, short-lived) ──────────

#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "abilities/face/face_task.h"
#include "abilities/ota/ota_codec.h"
#include "abilities/ota/ota_manager.h"
#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "link/ota_manifest.h"

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
  OtaUpdater()
      : face_(nullptr),
        state_(ota::State::kIdle),
        total_(0),
        received_(0),
        lastRxMs_(0),
        lastPct_(0),
        manifestSet_(false) {
    manifest_[0] = '\0';
  }

  // Progress door for the OLED bar (carrier trigger): the updater drives
  // FaceRenderer::showOta/hideOta/failOta through this pointer (FETCH fill,
  // VERIFY/COMMIT full bar, success hides, abort/failure alerts). Stored,
  // never owned; main() attaches after both are begun. Null-safe throughout.
  void attachFace(FaceRenderer* face) { face_ = face; }

  // Main-loop camera gate: while active, loop() skips CameraDriver::update()
  // (grab suspended — a JPEG grab mid-flash contends PSRAM/flash cache with
  // the single fb_count=1 buffer; see camera_driver.h). The sensor stays
  // powered (no deinit/re-init cycle); only the periodic grab is paused.
  bool active() const { return state_ != ota::State::kIdle; }

  // Carrier entry: pull only when the staged manifest says the image is
  // newer than the running HEAD_FW_VERSION. Manifest convention: the bin URL
  // ends in "/head.bin" and the manifest rides the sibling "/head-version"
  // path (derived by the shared otamanifest helper, single-sourced). No
  // manifest convention (other URLs) -> direct start() pull. A derivable but
  // unfetchable/unparsable manifest refuses the pull (fail-closed — the
  // operator repeats the trigger); an up-to-date manifest logs and skips
  // (not an error). Never logs URL bytes (versions + counters only).
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

  // Start an OTA from a plain-http URL. Works standalone — requires nothing
  // (link up OR down; association is the STA torrent below, not the B-link),
  // but logs the link state alongside for triage. Never logs the URL beyond
  // host + counters (parseUrl rejects '@', so no credentials can arrive).
  // Blocks briefly for connect + headers (WDT fed); the body pumps in poll().
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
    faceShow(0);  // FETCH: bar starts empty, fills per 10% step below
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
      otapolicy::applyEvent(state_, ota::Event::kChunk);
      uint32_t report = 0;
      if (otapolicy::progressDue(received_, total_, lastPct_, report)) {
        faceShow(static_cast<uint8_t>(report));  // FETCH: fill proportion
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
  // Blocking line read (start() only, never the pump): '\n'-terminated,
  // '\r' stripped, capped, 5s deadline with WDT feeds.
  bool readLine(char* buf, size_t cap) { return readLineFrom(client_, buf, cap); }

  // VERIFY -> COMMIT: manifest (when sent) then Update.end(). Success
  // reboots into the new slot; anything else aborts (old image boots).
  void finish() {
    otapolicy::applyEvent(state_, ota::Event::kStreamEnd);  // -> VERIFY
    faceShow(100);                                          // VERIFY: full bar
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
      faceFail();  // alert face, timeout back to the stored face
      client_.stop();
      return;
    }
#endif
    if (!manifestSet_) {
      LOG_I("OTA", "no manifest, size+magic only");
    }
    otapolicy::applyEvent(state_, ota::Event::kVerifyOk);  // -> COMMIT
    faceShow(100);                                         // COMMIT: full bar
    const ota::Event commit = otapolicy::commitOutcome(Update.end(true));
    otapolicy::applyEvent(state_, commit);
    if (commit == ota::Event::kCommitOk) {
      faceHide();  // success: stored face (the reboot follows)
      LOG_I("OTA", "ok bytes=%u rebooting", static_cast<unsigned>(received_));
      client_.stop();
      delay(100);
      ESP.restart();
    } else {
      LOG_W("OTA", "commit fail");
      faceFail();  // alert face, timeout back to the stored face
      client_.stop();
    }
  }

  void fail() {
    Update.abort();
    otapolicy::applyEvent(state_, ota::Event::kAbort);
    faceFail();  // alert face, timeout back to the stored face
    client_.stop();
  }

  // Manifest fetch for the carrier pull-gate (startIfNewer above):
  // plain-HTTP GET of the sibling /head-version text, Content-Length
  // required (manifests are tiny; chunked refused like the image path),
  // parsed by the shared otamanifest contract. Blocking with WDT feeds
  // (trigger context only, never the body pump). False on any failure
  // (fail-closed at the call site). Never logs URL bytes.
  bool fetchManifest(const char* murl, otamanifest::Manifest& man) {
    man.version = 0;
    man.size = 0;
    man.sha[0] = '\0';
    if (murl == nullptr) {
      return false;
    }
    // Re-validate the derived URL through the full grammar (single-sourced
    // with the CLI door) — never fetch a URL parseUrl rejects.
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

  // Blocking line read on any WiFiClient (start() + fetchManifest share it).
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

  // OLED progress hooks (null-safe): the bar lives only while attached.
  void faceShow(uint8_t pct) {
    if (face_ != nullptr) {
      face_->showOta(pct);
    }
  }

  void faceHide() {
    if (face_ != nullptr) {
      face_->hideOta();
    }
  }

  void faceFail() {
    if (face_ != nullptr) {
      face_->failOta();
    }
  }

  FaceRenderer* face_;
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
