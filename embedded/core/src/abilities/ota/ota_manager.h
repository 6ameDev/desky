#pragma once
// desky v2 OTA staging manager — abilities/ota (ability layer).
//
// ota::StagingManager: the upload state machine extracted verbatim from the
// former middleware/ota_staging.h (Slice 5), host-testable. Pure logic only:
// byte spans + plain POD views, zero sockets, zero mbedTLS, zero NVS.
//
// ── Seam ────────────────────────────────────────────────────────────────
// The firmware binder (ota_task.h) owns everything that needs firmware
// types and never lets it leak here: the AsyncWebServer routes, the mutex,
// the one permanent PSRAM buffer (ps_malloc, never freed once allocated so
// an in-flight GET can never dangle), and the mbedTLS SHA-256 context. It
// drives this manager in the exact order the former handler did:
//   index==0: parse ?version= via ota::parseVersionParam -> beginUpload ->
//             (unless rejected) allocate/reuse storage + start the digest ->
//             storageReady
//   chunks:   memcpy happens HERE (writeChunk) so the overflow rule below
//             stays byte-faithful; the task feeds the mbedTLS context with
//             the same bytes and reports the update outcome
//   final:    task finishes the digest -> finishUpload
//   POST-done / GETs: takePostResult / binView / versionView snapshots.
// Digest outcomes cross the seam as plain bools + a 32B array (null when
// the digest failed); the manager only tracks the shaValid_ flag and the
// hex string, exactly as before.
//
// Preserved semantics (any change here is a wire-visible behavior change):
// - One permanent buffer, never freed; chunks memcpy in place, so an
//   aborted/oversize upload leaves the committed prefix clobbered while
//   imgLen_/version_ still report the previous commit (faithful to the
//   original — the buffer is only ever read at the committed length).
// - Version applies atomically at commit via the shared
//   otamanifest::decideUploadVersion (missing keeps, valid adopts, malformed
//   rejects the whole upload with previous bytes + label preserved).
// - Oversize chunk: uploading_ stops, uploadLen_ resets, digest dies, the
//   commit later reports failure (uploadOk_ = false).
// - POST-done consumes uploadOk_ (read + clear) and reports the reject flag
//   separately; GETs refuse with busy while an upload streams.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "link/ota_manifest.h"
#include "ota_codec.h"

namespace ota {

// Outcome of storageReady(): the permanent buffer exists (kStarted) or the
// allocation failed (kNoStorage — the upload stops, commit later fails).
enum class StartResult : uint8_t { kStarted, kNoStorage };

// Outcome of writeChunk(): ignored (no upload in flight or bad args),
// accepted, or too big (upload stops, in-flight length resets, digest dies).
enum class WriteResult : uint8_t { kIgnored, kOk, kTooBig };

// POST-done snapshot (consumes uploadOk_, like the former handler).
struct PostResult {
  bool ok = false;
  size_t len = 0;
  bool rejected = false;
  uint32_t version = 0;
};

// GET /head.bin snapshot: busy wins over content; content needs a committed
// non-empty image. sha carries the stored hex verbatim (possibly empty).
struct BinView {
  bool busy = false;
  bool hasImage = false;
  const uint8_t* image = nullptr;
  size_t len = 0;
  uint32_t version = 0;
  char sha[otamanifest::kShaHexLen + 1] = {};
};

// GET /head-version snapshot: committed length (0 when no buffer yet) and
// the stored hex, or 64 '0' chars when no digest ever committed.
struct VersionView {
  uint32_t version = 0;
  size_t len = 0;
  char sha[otamanifest::kShaHexLen + 1] = {};
};

class StagingManager {
 public:
  StagingManager()
      : img_(nullptr),
        imgLen_(0),
        uploadLen_(0),
        uploading_(false),
        uploadOk_(false),
        version_(0),
        pendingVerVal_(0),
        pendingVerPresent_(false),
        pendingVerValid_(false),
        uploadRejected_(false),
        shaValid_(false) {
    shaHex_[0] = '\0';
  }

  // Manifest version: set ONLY by a committed POST ?version= (single source
  // of truth — no second channel). Safe pre-begin (stored, served later).
  uint32_t version() const { return version_; }

  size_t imageBytes() const { return imgLen_; }

  bool hasImage() const { return img_ != nullptr && imgLen_ > 0; }

  bool uploading() const { return uploading_; }

  bool hasSha() const { return shaHex_[0] != '\0'; }

  // Permanent staging buffer attached at storageReady(); exposed so the
  // task can reuse it (never freed) and serve it without copying. Null
  // until the first upload attaches one.
  uint8_t* imageBuffer() const { return img_; }

  // Upload start (index==0): records the pending ?version= label and decides
  // the reject up front. True = rejected (uploading_ forced false, previous
  // bytes + label preserved); false = proceed to storageReady. Never touches
  // the buffer or the digest flag — the task allocates/starts those next,
  // exactly as the former handler did after the version gate.
  bool beginUpload(bool verPresent, bool verValid, uint32_t verVal) {
    uploadRejected_ = false;
    pendingVerPresent_ = verPresent;
    pendingVerValid_ = verValid;
    pendingVerVal_ = verVal;
    if (verPresent && !verValid) {
      uploadRejected_ = true;
      uploading_ = false;
      return true;
    }
    return false;
  }

  // Storage + digest outcome for a non-rejected start: reuses the permanent
  // buffer (attaches `storage` on first use), arms the upload, records the
  // digest-start outcome. Null storage = allocation failed (upload stops;
  // the digest flag is left untouched, as before).
  StartResult storageReady(uint8_t* storage, bool digestOk) {
    if (img_ == nullptr) {
      img_ = storage;
    }
    uploading_ = true;
    uploadOk_ = false;
    uploadLen_ = 0;
    if (img_ == nullptr) {
      uploading_ = false;
      return StartResult::kNoStorage;
    }
    shaValid_ = digestOk;
    return StartResult::kStarted;
  }

  // One upload chunk: size gate, in-place memcpy, in-flight accounting. The
  // task feeds the same bytes to its digest and reports a failed update via
  // noteDigestFailed (the upload continues, the digest dies — as before).
  WriteResult writeChunk(const uint8_t* data, size_t len) {
    if (!uploading_ || img_ == nullptr || data == nullptr || len == 0) {
      return WriteResult::kIgnored;
    }
    if (uploadLen_ + len > kMaxImage) {
      uploading_ = false;
      uploadLen_ = 0;
      shaValid_ = false;
      return WriteResult::kTooBig;
    }
    memcpy(img_ + uploadLen_, data, len);
    uploadLen_ += len;
    return WriteResult::kOk;
  }

  void noteDigestFailed() { shaValid_ = false; }

  // Upload end (final): commits length + label atomically and records the
  // hex when the digest finished; a non-uploading final only fails the
  // pending POST-done. digest32 is the finished 32B digest, or null when
  // the digest failed (hex then clears, upload still commits — as before).
  // True = committed.
  bool finishUpload(bool digestOk, const uint8_t* digest32) {
    if (!uploading_) {
      uploadOk_ = false;
      return false;
    }
    uploading_ = false;
    imgLen_ = uploadLen_;
    // Atomic with the bytes: adopt the upload's label, keep previous when
    // the upload carried none (re-POST never resets the label).
    const otamanifest::VersionDecision vd =
        otamanifest::decideUploadVersion(pendingVerPresent_, pendingVerValid_, pendingVerVal_, version_);
    if (vd.action == otamanifest::VersionApply::kAdopt) {
      version_ = vd.version;
    }
    if (shaValid_ && digestOk && digest32 != nullptr) {
      shaHexFromDigest(digest32, shaHex_);
    } else {
      shaHex_[0] = '\0';
    }
    shaValid_ = false;
    uploadOk_ = true;
    return true;
  }

  // POST-done: snapshot + consume uploadOk_ (the reject flag is separate).
  PostResult takePostResult() {
    PostResult r;
    r.ok = uploadOk_;
    r.len = imgLen_;
    r.rejected = uploadRejected_;
    r.version = version_;
    uploadOk_ = false;
    return r;
  }

  BinView binView() const {
    BinView v;
    v.busy = uploading_;
    if (!v.busy && img_ != nullptr && imgLen_ > 0) {
      v.hasImage = true;
      v.image = img_;
      v.len = imgLen_;
      v.version = version_;
      for (size_t i = 0; i < otamanifest::kShaHexLen + 1; ++i) {
        v.sha[i] = shaHex_[i];
      }
    }
    return v;
  }

  VersionView versionView() const {
    VersionView v;
    v.version = version_;
    v.len = (img_ != nullptr) ? imgLen_ : 0;
    shaOrZeros(shaHex_[0] != '\0', shaHex_, v.sha);
    return v;
  }

 private:
  // One permanent buffer (never freed once attached — see above).
  uint8_t* img_;
  size_t imgLen_;
  size_t uploadLen_;
  bool uploading_;
  bool uploadOk_;
  uint32_t version_;
  // Pending label from the in-flight POST (?version=): applied atomically at
  // commit, never ahead of the bytes. Reset on every upload start.
  uint32_t pendingVerVal_;
  bool pendingVerPresent_;
  bool pendingVerValid_;
  bool uploadRejected_;
  char shaHex_[otamanifest::kShaHexLen + 1];
  bool shaValid_;
};

}  // namespace ota
