#pragma once
// OTA staging codec — abilities/ota.
//
// Pure wire/format vocabulary for the carrier head-image staging server:
// route paths, content types, size caps from the CFG_OTA_* keys, response
// message literals, the POST ?version= grammar, and the SHA-hex helpers.
// Arduino-free (stdint/stddef only) so host tests include it directly.
// Version single-source rule stays in the shared contract
// (link/ota_manifest.h decideUploadVersion — never re-derived here).

#include <stddef.h>
#include <stdint.h>

#include "link/ota_manifest.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
#ifndef CFG_OTA_STAGE_PORT
#define CFG_OTA_STAGE_PORT 8080
#endif
#ifndef CFG_OTA_STAGE_MAX_BYTES
#define CFG_OTA_STAGE_MAX_BYTES 0x200000
#endif

namespace ota {

// Server port + PSRAM staging cap (head slot is 0x1F0000; margin above).
constexpr uint16_t kPort = CFG_OTA_STAGE_PORT;
constexpr size_t kMaxImage = CFG_OTA_STAGE_MAX_BYTES;

// Routes: POST /head.bin[?version=N] uploads, GET /head.bin serves bytes,
// GET /head-version serves the manifest text.
constexpr char kRouteBin[] = "/head.bin";
constexpr char kRouteVersion[] = "/head-version";
constexpr char kParamVersion[] = "version";

// Content types: octet-stream for the image (the head updater requires an
// explicit Content-Length and refuses chunked encoding), text for the rest.
constexpr char kTypeBin[] = "application/octet-stream";
constexpr char kTypeText[] = "text/plain";

// Response bodies.
constexpr char kMsgBusy[] = "upload in progress";                    // 409: GET while a POST streams
constexpr char kMsgEmpty[] = "no image staged";                      // 404: GET with nothing committed
constexpr char kMsgManifestFail[] = "manifest build failed";         // 500: buildManifest refused
constexpr char kMsgBadVersion[] = "bad version (params preserved)";  // 400: malformed ?version=
constexpr char kMsgUploadFail[] = "upload failed";                   // 500: POST done with nothing committed

// Header names on GET /head.bin.
constexpr char kHeaderVersion[] = "X-Firmware-Version";
constexpr char kHeaderSha[] = "X-Firmware-Sha256";

// POST ?version= grammar: 1..10 ASCII chars, strict u32 decimal via the
// shared otamanifest::parseU32. Longer or non-decimal values reject (the
// whole upload rejects, previous bytes + label preserved).
inline bool parseVersionParam(const char* s, size_t len, uint32_t& out) {
  if (s == nullptr || len == 0 || len > 10) {
    return false;
  }
  char vb[12];
  for (size_t i = 0; i < len; ++i) {
    vb[i] = s[i];
  }
  vb[len] = '\0';
  return otamanifest::parseU32(vb, out);
}

// Lowercase hex of a 32B digest. out must hold kShaHexLen + 1.
inline void shaHexFromDigest(const uint8_t digest[32], char* outHex65) {
  if (digest == nullptr || outHex65 == nullptr) {
    return;
  }
  static const char kHex[] = "0123456789abcdef";
  for (size_t k = 0; k < 32; ++k) {
    outHex65[2 * k] = kHex[(digest[k] >> 4) & 0xF];
    outHex65[2 * k + 1] = kHex[digest[k] & 0xF];
  }
  outHex65[otamanifest::kShaHexLen] = '\0';
}

// Manifest sha field for GET /head-version: the stored hex when an upload
// committed one, else 64 '0' chars (version=0 reads as up-to-date -
// fail-closed).
inline void shaOrZeros(bool haveHex, const char* storedHex65, char* outHex65) {
  if (outHex65 == nullptr) {
    return;
  }
  if (haveHex && storedHex65 != nullptr) {
    for (size_t i = 0; i < otamanifest::kShaHexLen + 1; ++i) {
      outHex65[i] = storedHex65[i];
    }
    return;
  }
  for (size_t i = 0; i < otamanifest::kShaHexLen; ++i) {
    outHex65[i] = '0';
  }
  outHex65[otamanifest::kShaHexLen] = '\0';
}

}  // namespace ota
