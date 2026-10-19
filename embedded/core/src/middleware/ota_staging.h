#pragma once
// Carrier OTA staging server — header-only, firmware-only (ESPAsyncWebServer).
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
// bytes + label preserved. The `otahead <url>` console verb only triggers.
//
// The `otahead <url>` console verb stages the link URL; the manifest version
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

// Fallbacks keep this header compiling if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_OTA_STAGE_PORT
#define CFG_OTA_STAGE_PORT 8080
#endif
#ifndef CFG_OTA_STAGE_MAX_BYTES
#define CFG_OTA_STAGE_MAX_BYTES 0x200000
#endif

class OtaStaging {
 public:
  static constexpr uint16_t kPort = CFG_OTA_STAGE_PORT;
  static constexpr size_t kMaxImage = CFG_OTA_STAGE_MAX_BYTES;

  OtaStaging()
      : server_(kPort), mutex_(nullptr), img_(nullptr), imgLen_(0), uploading_(false), uploadOk_(false), version_(0) {
    shaHex_[0] = '\0';
  }

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
    server_.on("/head.bin", HTTP_GET, [this](AsyncWebServerRequest* req) { handleGetBin(req); });
    server_.on("/head-version", HTTP_GET, [this](AsyncWebServerRequest* req) { handleGetVersion(req); });
    server_.on(
        "/head.bin", HTTP_POST, [this](AsyncWebServerRequest* req) { handlePostDone(req); },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len,
               bool final) { handleUpload(req, filename, index, data, len, final); });
    server_.begin();
    LOG_I("BOOT", "ota staging ready port=%u cap=%u", static_cast<unsigned>(kPort), static_cast<unsigned>(kMaxImage));
    return true;
  }

  // Manifest version for GET /head-version + the X-Firmware-Version header.
  // Set ONLY by POST ?version= (single source of truth — no second channel).
  // Safe pre-begin (stored, served later).
  uint32_t version() const { return version_; }

  size_t imageBytes() const { return imgLen_; }

  bool hasImage() const { return img_ != nullptr && imgLen_ > 0; }

 private:
  void handleGetBin(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    uint8_t* img = nullptr;
    size_t len = 0;
    uint32_t version = 0;
    char sha[otamanifest::kShaHexLen + 1] = {};
    bool busy = false;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      busy = uploading_;
      if (!busy && img_ != nullptr && imgLen_ > 0) {
        img = img_;
        len = imgLen_;
        version = version_;
        for (size_t i = 0; i < otamanifest::kShaHexLen + 1; ++i) {
          sha[i] = shaHex_[i];
        }
      }
      xSemaphoreGive(mutex_);
    }
    if (busy) {
      req->send(409, "text/plain", "upload in progress");
      return;
    }
    if (img == nullptr || len == 0) {
      req->send(404, "text/plain", "no image staged");
      return;
    }
    // No copy: streams the stable PSRAM buffer via memcpy_P (plain memcpy on
    // ESP32) with an explicit Content-Length (the head updater requires it
    // and refuses chunked encoding).
    AsyncWebServerResponse* res = req->beginResponse(200, "application/octet-stream", img, len);
    char verStr[12];
    snprintf(verStr, sizeof(verStr), "%lu", static_cast<unsigned long>(version));
    res->addHeader("X-Firmware-Version", verStr);
    res->addHeader("X-Firmware-Sha256", sha);
    req->send(res);
  }

  void handleGetVersion(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    uint32_t version = 0;
    size_t len = 0;
    char sha[otamanifest::kShaHexLen + 1] = {};
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      version = version_;
      len = (img_ != nullptr) ? imgLen_ : 0;
      if (shaHex_[0] != '\0') {
        for (size_t i = 0; i < otamanifest::kShaHexLen + 1; ++i) {
          sha[i] = shaHex_[i];
        }
      } else {
        for (size_t i = 0; i < otamanifest::kShaHexLen; ++i) {
          sha[i] = '0';
        }
        sha[otamanifest::kShaHexLen] = '\0';
      }
      xSemaphoreGive(mutex_);
    }
    char body[otamanifest::kManifestCap];
    if (!otamanifest::buildManifest(version, static_cast<uint32_t>(len), sha, body, sizeof(body))) {
      req->send(500, "text/plain", "manifest build failed");
      return;
    }
    req->send(200, "text/plain", body);
  }

  void handlePostDone(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    bool ok = false;
    size_t len = 0;
    bool rejected = false;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      ok = uploadOk_;
      len = imgLen_;
      rejected = uploadRejected_;
      uploadOk_ = false;
      xSemaphoreGive(mutex_);
    }
    if (rejected) {
      req->send(400, "text/plain", "bad version (params preserved)");
      return;
    }
    if (!ok) {
      req->send(500, "text/plain", "upload failed");
      return;
    }
    // Counters only: length + stored label, never content.
    uint32_t version = 0;
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      version = version_;
      xSemaphoreGive(mutex_);
    }
    char body[64];
    snprintf(body, sizeof(body), "staged bytes=%u version=%lu", static_cast<unsigned>(len),
             static_cast<unsigned long>(version));
    req->send(200, "text/plain", body);
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
      uploadRejected_ = false;
      pendingVerPresent_ = false;
      pendingVerValid_ = false;
      pendingVerVal_ = 0;
      if (req != nullptr && req->hasParam("version")) {
        const AsyncWebParameter* pv = req->getParam("version");
        pendingVerPresent_ = true;
        if (pv != nullptr) {
          const String& vs = pv->value();
          const size_t vl = vs.length();
          if (vl > 0 && vl <= 10) {
            char vb[12];
            for (size_t i = 0; i < vl; ++i) {
              vb[i] = vs.charAt(i);
            }
            vb[vl] = '\0';
            uint32_t vv = 0;
            if (otamanifest::parseU32(vb, vv)) {
              pendingVerValid_ = true;
              pendingVerVal_ = vv;
            }
          }
        }
        if (!pendingVerValid_) {
          uploadRejected_ = true;
          uploading_ = false;
          LOG_W("OTA", "stage bad version (params preserved)");
        }
      }
      // Start: (re)use the one permanent PSRAM buffer — never freed, so an
      // in-flight GET can never dangle. Skipped wholesale on version reject
      // (previous bytes + label preserved, never half-written).
      if (!uploadRejected_) {
        if (img_ == nullptr) {
          img_ = static_cast<uint8_t*>(ps_malloc(kMaxImage));
        }
        uploading_ = true;
        uploadOk_ = false;
        uploadLen_ = 0;
        if (img_ == nullptr) {
          uploading_ = false;
          LOG_W("OTA", "stage no psram (want cap=%u)", static_cast<unsigned>(kMaxImage));
        } else {
          const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
          shaValid_ = (md != nullptr);
          if (shaValid_) {
            mbedtls_md_init(&shaCtx_);
            if (mbedtls_md_setup(&shaCtx_, md, 0) != 0) {
              shaValid_ = false;
            } else if (mbedtls_md_starts(&shaCtx_) != 0) {
              mbedtls_md_free(&shaCtx_);
              shaValid_ = false;
            }
          }
        }
      }
    }
    if (uploading_ && img_ != nullptr && data != nullptr && len > 0) {
      if (uploadLen_ + len > kMaxImage) {
        uploading_ = false;
        uploadLen_ = 0;
        if (shaValid_) {
          mbedtls_md_free(&shaCtx_);
          shaValid_ = false;
        }
        LOG_W("OTA", "stage too big (cap=%u)", static_cast<unsigned>(kMaxImage));
      } else {
        memcpy(img_ + uploadLen_, data, len);
        uploadLen_ += len;
        if (shaValid_) {
          if (mbedtls_md_update(&shaCtx_, data, len) != 0) {
            mbedtls_md_free(&shaCtx_);
            shaValid_ = false;
          }
        }
      }
    }
    if (final && uploading_) {
      uploading_ = false;
      imgLen_ = uploadLen_;
      // Atomic with the bytes: adopt the upload's label, keep previous when
      // the upload carried none (re-POST never resets the label).
      const otamanifest::VersionDecision vd =
          otamanifest::decideUploadVersion(pendingVerPresent_, pendingVerValid_, pendingVerVal_, version_);
      if (vd.action == otamanifest::VersionApply::kAdopt) {
        version_ = vd.version;
      }
      uint8_t digest[32] = {};
      if (shaValid_ && mbedtls_md_finish(&shaCtx_, digest) == 0) {
        static const char kHex[] = "0123456789abcdef";
        for (size_t k = 0; k < 32; ++k) {
          shaHex_[2 * k] = kHex[(digest[k] >> 4) & 0xF];
          shaHex_[2 * k + 1] = kHex[digest[k] & 0xF];
        }
        shaHex_[otamanifest::kShaHexLen] = '\0';
        mbedtls_md_free(&shaCtx_);
        shaValid_ = false;
      } else {
        shaHex_[0] = '\0';
        if (shaValid_) {
          mbedtls_md_free(&shaCtx_);
          shaValid_ = false;
        }
      }
      mbedtls_platform_zeroize(digest, sizeof(digest));
      uploadOk_ = true;
      LOG_I("OTA", "staged bytes=%u version=%lu sha=%d", static_cast<unsigned>(imgLen_),
            static_cast<unsigned long>(version_), shaHex_[0] != '\0' ? 1 : 0);
    } else if (final) {
      uploadOk_ = false;
    }
    xSemaphoreGive(mutex_);
  }

  AsyncWebServer server_;
  SemaphoreHandle_t mutex_;
  // One permanent PSRAM buffer (never freed once allocated — see above).
  uint8_t* img_;
  size_t imgLen_;
  size_t uploadLen_ = 0;
  bool uploading_;
  bool uploadOk_;
  uint32_t version_;
  // Pending label from the in-flight POST (?version=): applied atomically at
  // commit, never ahead of the bytes. Reset on every upload start.
  uint32_t pendingVerVal_ = 0;
  bool pendingVerPresent_ = false;
  bool pendingVerValid_ = false;
  bool uploadRejected_ = false;
  char shaHex_[otamanifest::kShaHexLen + 1];
  mbedtls_md_context_t shaCtx_;
  bool shaValid_ = false;
};

#endif  // ARDUINO
