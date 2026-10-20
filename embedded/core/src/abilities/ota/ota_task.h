#pragma once
// Carrier OTA staging server — abilities/ota (ability layer, Slice 5).
//
// Firmware-only HTTP binder owning an ota::StagingManager and delegating to
// it. Moved verbatim from middleware/ota_staging.h; zero behavior or wire
// change — routes, caps, version semantics, and state transitions are
// byte-identical (see ota_codec.h / ota_manager.h).
//
// Serves the head image the carrier pulls over the link-triggered OTA:
//   POST /head.bin[?version=N] upload a head image into PSRAM staging (2MB cap)
//   GET  /head.bin      staged image bytes, Content-Length + X-Firmware-*
//                       manifest headers (the head updater requires a length)
//   GET  /head-version  manifest text (version/size/sha256 lines, built by
//                       the shared otamanifest::buildManifest contract)
//
// Version rides the upload (single source of truth — no second channel):
// ?version=N adopts the label atomically with the bytes; no ?version keeps
// the previous label; malformed ?version rejects the upload with previous
// bytes + label preserved. The `otahead` console verb only triggers.
//
// The `otahead` console verb stages the link URL here (Slice 6c: the old
// LinkServer stage lives in this ability now); the manifest version
// arrives only via POST ?version= (single source). An empty stage serves
// version=0 (the head pull-gate treats it as up-to-date and never pulls —
// fail-closed by construction).
//
// Buffer: one kMaxImage PSRAM allocation, kept forever once allocated (the
// GET fill path streams it without copying, so it is never freed — no
// use-after-free across the async upload/serve interleavings). Uploads while
// a serve is in flight are refused (409); concurrent GETs share the stable
// pointer + a length snapshot. Async handlers run on the async TCP task, so
// no WDT subscription is needed here; the head-side pump feeds its own WDT.
// String use is confined to the AsyncWebServer handler boundary (its API is
// String-based); the link RX/TX data paths stay String-free per the rules.

#include "ota_manager.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include "common/fault_manager.h"
#include "common/logger.h"
#include "config.h"
#include "link/ota_manifest.h"
#include "middleware/link_server.h"

class OtaStaging {
 public:
  static constexpr uint16_t kPort = ota::kPort;
  static constexpr size_t kMaxImage = ota::kMaxImage;

  OtaStaging() : server_(kPort), mutex_(nullptr), shaActive_(false) {}

  // Binds the staging routes + starts the server. Call once in setup(),
  // AFTER UdpServer::begin (sockets ride the already-up stack; no WiFi mode
  // owned here). False + caller LOG_W on mutex failure — the caller continues
  // (the `otahead` verb fails closed without staging, driving never bricks).
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    DESKY_ASSERT(mutex_ != nullptr);
    if (mutex_ == nullptr) {
      return false;
    }
    server_.on(ota::kRouteBin, HTTP_GET, [this](AsyncWebServerRequest* req) { handleGetBin(req); });
    server_.on(ota::kRouteVersion, HTTP_GET, [this](AsyncWebServerRequest* req) { handleGetVersion(req); });
    server_.on(
        ota::kRouteBin, HTTP_POST, [this](AsyncWebServerRequest* req) { handlePostDone(req); },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len,
               bool final) { handleUpload(req, filename, index, data, len, final); });
    server_.begin();
    LOG_I("BOOT", "ota staging ready port=%u cap=%u", static_cast<unsigned>(kPort), static_cast<unsigned>(kMaxImage));
    return true;
  }

  // Manifest version for GET /head-version + the X-Firmware-Version header.
  // Set ONLY by POST ?version= (single source of truth — no second channel).
  // Safe pre-begin (stored, served later).
  uint32_t version() const { return mgr_.version(); }

  size_t imageBytes() const { return mgr_.imageBytes(); }

  bool hasImage() const { return mgr_.hasImage(); }

  // Slice 6c link-delivery stage: ability-side home of the old
  // LinkServer::stageOtaUrl/clearOta/stagedOtaLen trio (policy side, never
  // sockets). Stage a carrier OTA URL for the head pull, fed by the
  // `otahead <url>` console verb; copies under lock. The main-loop pump
  // below unicasts it once as a kTypeOta frame to the HB-pinned head.
  // Validation is the shared otamanifest grammar (1..128 printable-ASCII
  // bytes, no '@'/whitespace — credentials never ride or log);
  // invalid/null/pre-begin -> false, fail-closed. Every valid command
  // REPLACES the stage and re-arms the send (console-verb semantics: latest
  // command wins — a head mid-download ignores the re-trigger via its
  // in-flight guard, an idle head pulls the newest URL).
  bool stageOtaUrl(const char* url) {
    if (url == nullptr || mutex_ == nullptr) {
      return false;
    }
    size_t len = 0;
    while (len <= otamanifest::kUrlMax && url[len] != '\0') {
      ++len;
    }
    if (len == 0 || len > otamanifest::kUrlMax) {
      return false;
    }
    if (!otamanifest::isValidOtaUrlBytes(reinterpret_cast<const uint8_t*>(url), len)) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (size_t i = 0; i < len; ++i) {
      stagedOta_[i] = static_cast<uint8_t>(url[i]);
    }
    stagedOtaLen_ = len;
    stagedOtaValid_ = true;
    otaSent_ = false;
    xSemaphoreGive(mutex_);
    return true;
  }

  // Drop any staged/unsent OTA (torn download repeat path) and unlatch the
  // once-per-command gate. Safe pre-begin.
  void clearOta() {
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    stagedOtaValid_ = false;
    otaSent_ = false;
    stagedOtaLen_ = 0;
    for (size_t i = 0; i < linkcode::kOtaUrlMaxLen; ++i) {
      stagedOta_[i] = 0;
    }
    xSemaphoreGive(mutex_);
  }

  // Staged OTA length for the console door (counters only). 0 pre-begin,
  // or when nothing is staged.
  size_t stagedOtaLen() const {
    if (mutex_ == nullptr) {
      return 0;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const size_t len = stagedOtaValid_ ? stagedOtaLen_ : 0;
    xSemaphoreGive(mutex_);
    return len;
  }

  // Slice 6c OTA link pump: called from the main loop (replaces the old
  // link-task trySendOta). Hands the staged URL to the transport's
  // pinned-peer sendUnicast exactly once per `otahead` command (otaSent_
  // latch, same linkserver::otaReadyToSend gate the old task path used).
  // No head linked -> silent wait for a late joiner; nothing staged or
  // already sent -> no-op. Consume-on-attempt: the latch sets once a peer
  // is present, even if the radio refuses the datagram — same as the old
  // task path (a failed unicast is dropped for this command, never
  // retried; the next `otahead` re-arms via stageOtaUrl). Returns true iff
  // a frame was handed to the radio. Never touches sockets; never logs URL
  // bytes (length only).
  bool pumpOtaLink(LinkServer& link) {
    if (mutex_ == nullptr) {
      return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool valid = stagedOtaValid_;
    const bool sent = otaSent_;
    xSemaphoreGive(mutex_);
    IPAddress peer;
    uint16_t port = 0;
    const bool havePeer = link.headPeer(peer, port);
    if (!linkserver::otaReadyToSend(valid, sent, havePeer)) {
      if (valid && !sent && !havePeer) {
        LOG_EVERY_N(20, "LINK", "ota pending, no head peer");
      }
      return false;
    }
    uint8_t staged[linkcode::kOtaUrlMaxLen] = {};
    size_t stagedLen = 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool doSend = stagedOtaValid_ && !otaSent_;
    if (doSend) {
      for (size_t i = 0; i < stagedOtaLen_; ++i) {
        staged[i] = stagedOta_[i];
      }
      stagedLen = stagedOtaLen_;
      stagedOtaValid_ = false;
      otaSent_ = true;
    }
    xSemaphoreGive(mutex_);
    if (!doSend) {
      return false;
    }
    uint16_t seq = 0;
    if (!link.sendUnicast(linkcode::kTypeOta, staged, static_cast<uint16_t>(stagedLen), seq)) {
      return false;
    }
    LOG_I("LINK", "ota sent seq=%u len=%u", static_cast<unsigned>(seq), static_cast<unsigned>(stagedLen));
    return true;
  }

 private:
  void handleGetBin(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    ota::BinView v;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      v = mgr_.binView();
      xSemaphoreGive(mutex_);
    }
    if (v.busy) {
      req->send(409, ota::kTypeText, ota::kMsgBusy);
      return;
    }
    if (!v.hasImage) {
      req->send(404, ota::kTypeText, ota::kMsgEmpty);
      return;
    }
    // No copy: streams the stable PSRAM buffer via memcpy_P (plain memcpy on
    // ESP32) with an explicit Content-Length (the head updater requires it
    // and refuses chunked encoding).
    AsyncWebServerResponse* res = req->beginResponse(200, ota::kTypeBin, v.image, v.len);
    char verStr[12];
    snprintf(verStr, sizeof(verStr), "%lu", static_cast<unsigned long>(v.version));
    res->addHeader(ota::kHeaderVersion, verStr);
    res->addHeader(ota::kHeaderSha, v.sha);
    req->send(res);
  }

  void handleGetVersion(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    ota::VersionView v;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      v = mgr_.versionView();
      xSemaphoreGive(mutex_);
    }
    char body[otamanifest::kManifestCap];
    if (!otamanifest::buildManifest(v.version, static_cast<uint32_t>(v.len), v.sha, body, sizeof(body))) {
      req->send(500, ota::kTypeText, ota::kMsgManifestFail);
      return;
    }
    req->send(200, ota::kTypeText, body);
  }

  void handlePostDone(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    ota::PostResult r;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      r = mgr_.takePostResult();
      xSemaphoreGive(mutex_);
    }
    if (r.rejected) {
      req->send(400, ota::kTypeText, ota::kMsgBadVersion);
      return;
    }
    if (!r.ok) {
      req->send(500, ota::kTypeText, ota::kMsgUploadFail);
      return;
    }
    // Counters only: length + stored label, never content.
    char body[64];
    snprintf(body, sizeof(body), "staged bytes=%u version=%lu", static_cast<unsigned>(r.len),
             static_cast<unsigned long>(r.version));
    req->send(200, ota::kTypeText, body);
  }

  void handleUpload(AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len,
                    bool final) {
    (void)req;
    (void)filename;
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (index == 0) {
      // Single source of truth: the version rides the upload (?version=N).
      // Missing -> keep previous label (re-POST never resets it); malformed ->
      // reject the whole upload up front, previous bytes + label preserved.
      // Query read only (never URL-logged beyond counters at the end).
      bool present = false;
      bool valid = false;
      uint32_t val = 0;
      if (req != nullptr && req->hasParam(ota::kParamVersion)) {
        present = true;
        const AsyncWebParameter* pv = req->getParam(ota::kParamVersion);
        if (pv != nullptr) {
          const String& vs = pv->value();
          valid = ota::parseVersionParam(vs.c_str(), vs.length(), val);
        }
      }
      if (mgr_.beginUpload(present, valid, val)) {
        LOG_W("OTA", "stage bad version (params preserved)");
      } else {
        // Start: (re)use the one permanent PSRAM buffer — never freed, so an
        // in-flight GET can never dangle. Skipped wholesale on version reject
        // (previous bytes + label preserved, never half-written).
        uint8_t* storage = mgr_.imageBuffer();
        if (storage == nullptr) {
          storage = static_cast<uint8_t*>(ps_malloc(kMaxImage));
        }
        if (storage == nullptr) {
          mgr_.storageReady(nullptr, false);
          LOG_W("OTA", "stage no psram (want cap=%u)", static_cast<unsigned>(kMaxImage));
        } else {
          const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
          bool digestOk = (md != nullptr);
          if (digestOk) {
            mbedtls_md_init(&shaCtx_);
            if (mbedtls_md_setup(&shaCtx_, md, 0) != 0) {
              digestOk = false;
            } else if (mbedtls_md_starts(&shaCtx_) != 0) {
              mbedtls_md_free(&shaCtx_);
              digestOk = false;
            }
          }
          shaActive_ = digestOk;
          mgr_.storageReady(storage, digestOk);
        }
      }
    }
    if (mgr_.uploading()) {
      const ota::WriteResult wr = mgr_.writeChunk(data, len);
      if (wr == ota::WriteResult::kTooBig) {
        if (shaActive_) {
          mbedtls_md_free(&shaCtx_);
          shaActive_ = false;
        }
        mgr_.noteDigestFailed();
        LOG_W("OTA", "stage too big (cap=%u)", static_cast<unsigned>(kMaxImage));
      } else if (wr == ota::WriteResult::kOk && shaActive_) {
        if (mbedtls_md_update(&shaCtx_, data, len) != 0) {
          mbedtls_md_free(&shaCtx_);
          shaActive_ = false;
          mgr_.noteDigestFailed();
        }
      }
    }
    if (final) {
      if (mgr_.uploading()) {
        uint8_t digest[32] = {};
        bool digestOk = false;
        if (shaActive_) {
          digestOk = (mbedtls_md_finish(&shaCtx_, digest) == 0);
          mbedtls_md_free(&shaCtx_);
          shaActive_ = false;
        }
        const bool committed = mgr_.finishUpload(digestOk, digestOk ? digest : nullptr);
        (void)committed;
        mbedtls_platform_zeroize(digest, sizeof(digest));
        LOG_I("OTA", "staged bytes=%u version=%lu sha=%d", static_cast<unsigned>(mgr_.imageBytes()),
              static_cast<unsigned long>(mgr_.version()), mgr_.hasSha() ? 1 : 0);
      } else {
        mgr_.finishUpload(false, nullptr);
      }
    }
    xSemaphoreGive(mutex_);
  }

  AsyncWebServer server_;
  SemaphoreHandle_t mutex_;
  ota::StagingManager mgr_;
  mbedtls_md_context_t shaCtx_;
  bool shaActive_;
  // Slice 6c link-delivery stage (policy side, never sockets): the
  // `otahead` console verb stages the pull URL here; pumpOtaLink above
  // unicasts it ONCE to the pinned head (otaSent_ latches until the next
  // stageOtaUrl / clearOta). Same shape as the pairing QR stage; the URL
  // bytes are validated by stageOtaUrl and never logged. All writers run
  // on the main loop (console poll + pump), guarded by mutex_ like the
  // staging manager state above.
  uint8_t stagedOta_[linkcode::kOtaUrlMaxLen] = {};
  size_t stagedOtaLen_ = 0;
  bool stagedOtaValid_ = false;
  bool otaSent_ = false;
};

#endif  // ARDUINO
