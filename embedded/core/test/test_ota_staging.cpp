// Host unit tests for the carrier OTA staging ability (no hardware).
// Run: pio test -e native-test
//
// Covers the Slice 5 extraction (abilities/ota triple): the codec wire
// vocabulary (routes, caps, ?version= grammar, SHA helpers) and the manager
// state machine (begin-chunk/finish/abort/version transitions) byte-for-byte
// against the former middleware server. HTTP binding (ota_task.h) stays
// firmware-only. Runner lives in test_udp_codec.cpp.

#include <string.h>
#include <unity.h>

#include "../src/abilities/ota/ota_manager.h"

namespace {
const uint8_t kFakeDigest[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                                 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
const char kFakeHex[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

// Commits `len` bytes from `buf` with an optional ?version= label into a
// fresh manager; returns the finishUpload verdict. Digest succeeds.
bool commitBytes(ota::StagingManager& m, uint8_t* buf, const uint8_t* bytes, size_t len, bool verPresent, bool verValid,
                 uint32_t verVal) {
  if (m.beginUpload(verPresent, verValid, verVal)) {
    return false;
  }
  if (m.storageReady(buf, true) != ota::StartResult::kStarted) {
    return false;
  }
  if (len > 0 && m.writeChunk(bytes, len) != ota::WriteResult::kOk) {
    return false;
  }
  return m.finishUpload(true, kFakeDigest);
}
}  // namespace

void test_ota_codec_routes_and_caps() {
  TEST_ASSERT_EQUAL_STRING("/head.bin", ota::kRouteBin);
  TEST_ASSERT_EQUAL_STRING("/head-version", ota::kRouteVersion);
  TEST_ASSERT_EQUAL_STRING("version", ota::kParamVersion);
  TEST_ASSERT_EQUAL_STRING("application/octet-stream", ota::kTypeBin);
  TEST_ASSERT_EQUAL_STRING("text/plain", ota::kTypeText);
  TEST_ASSERT_EQUAL_STRING("X-Firmware-Version", ota::kHeaderVersion);
  TEST_ASSERT_EQUAL_STRING("X-Firmware-Sha256", ota::kHeaderSha);
  TEST_ASSERT_EQUAL_STRING("upload in progress", ota::kMsgBusy);
  TEST_ASSERT_EQUAL_STRING("no image staged", ota::kMsgEmpty);
  TEST_ASSERT_EQUAL_STRING("manifest build failed", ota::kMsgManifestFail);
  TEST_ASSERT_EQUAL_STRING("bad version (params preserved)", ota::kMsgBadVersion);
  TEST_ASSERT_EQUAL_STRING("upload failed", ota::kMsgUploadFail);
  TEST_ASSERT_EQUAL_UINT16(8080, ota::kPort);
  TEST_ASSERT_EQUAL_size_t(0x200000, ota::kMaxImage);
}

void test_ota_codec_parse_version_param() {
  uint32_t v = 0;
  TEST_ASSERT_TRUE(ota::parseVersionParam("2", 1, v));
  TEST_ASSERT_EQUAL_UINT32(2, v);
  TEST_ASSERT_TRUE(ota::parseVersionParam("0", 1, v));
  TEST_ASSERT_EQUAL_UINT32(0, v);
  TEST_ASSERT_TRUE(ota::parseVersionParam("0007", 4, v));
  TEST_ASSERT_EQUAL_UINT32(7, v);
  TEST_ASSERT_TRUE(ota::parseVersionParam("4294967295", 10, v));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, v);
  TEST_ASSERT_FALSE(ota::parseVersionParam("4294967296", 10, v));  // u32 overflow
  TEST_ASSERT_FALSE(ota::parseVersionParam("", 0, v));             // empty
  TEST_ASSERT_FALSE(ota::parseVersionParam(nullptr, 1, v));
  TEST_ASSERT_FALSE(ota::parseVersionParam("12345678901", 11, v));  // > 10 chars
  TEST_ASSERT_FALSE(ota::parseVersionParam("12a", 3, v));
  TEST_ASSERT_FALSE(ota::parseVersionParam("-1", 2, v));
  TEST_ASSERT_FALSE(ota::parseVersionParam(" 2", 2, v));
}

void test_ota_codec_sha_helpers() {
  char hex[otamanifest::kShaHexLen + 1];
  ota::shaHexFromDigest(kFakeDigest, hex);
  TEST_ASSERT_EQUAL_STRING(kFakeHex, hex);
  char zeros[otamanifest::kShaHexLen + 1];
  ota::shaOrZeros(false, hex, zeros);
  TEST_ASSERT_EQUAL_STRING("0000000000000000000000000000000000000000000000000000000000000000", zeros);
  char copy[otamanifest::kShaHexLen + 1];
  ota::shaOrZeros(true, hex, copy);
  TEST_ASSERT_EQUAL_STRING(kFakeHex, copy);
  ota::shaHexFromDigest(nullptr, copy);  // null-safe, no crash
  ota::shaHexFromDigest(kFakeDigest, nullptr);
  ota::shaOrZeros(true, hex, nullptr);
}

void test_ota_mgr_empty_views() {
  ota::StagingManager m;
  TEST_ASSERT_EQUAL_UINT32(0, m.version());
  TEST_ASSERT_EQUAL_size_t(0, m.imageBytes());
  TEST_ASSERT_FALSE(m.hasImage());
  TEST_ASSERT_FALSE(m.uploading());
  const ota::BinView b = m.binView();
  TEST_ASSERT_FALSE(b.busy);
  TEST_ASSERT_FALSE(b.hasImage);
  const ota::VersionView v = m.versionView();
  TEST_ASSERT_EQUAL_UINT32(0, v.version);
  TEST_ASSERT_EQUAL_size_t(0, v.len);
  TEST_ASSERT_EQUAL_STRING("0000000000000000000000000000000000000000000000000000000000000000", v.sha);
  const ota::PostResult r = m.takePostResult();
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_FALSE(r.rejected);
  TEST_ASSERT_EQUAL_size_t(0, r.len);
}

void test_ota_mgr_commit_adopts_version() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t img[] = {'h', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_TRUE(commitBytes(m, buf, img, sizeof(img), true, true, 5));
  TEST_ASSERT_EQUAL_UINT32(5, m.version());
  TEST_ASSERT_EQUAL_size_t(5, m.imageBytes());
  TEST_ASSERT_TRUE(m.hasImage());
  TEST_ASSERT_TRUE(m.hasSha());
  const ota::BinView b = m.binView();
  TEST_ASSERT_FALSE(b.busy);
  TEST_ASSERT_TRUE(b.hasImage);
  TEST_ASSERT_EQUAL_size_t(5, b.len);
  TEST_ASSERT_EQUAL_UINT32(5, b.version);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(img, b.image, sizeof(img));
  TEST_ASSERT_EQUAL_STRING(kFakeHex, b.sha);
  const ota::PostResult r = m.takePostResult();
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.rejected);
  TEST_ASSERT_EQUAL_size_t(5, r.len);
  TEST_ASSERT_EQUAL_UINT32(5, r.version);
  const ota::PostResult r2 = m.takePostResult();  // consumed: read + clear
  TEST_ASSERT_FALSE(r2.ok);
}

void test_ota_mgr_no_version_keeps_label() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t a[] = {'a'};
  const uint8_t b2[] = {'b', 'c'};
  TEST_ASSERT_TRUE(commitBytes(m, buf, a, sizeof(a), true, true, 5));
  TEST_ASSERT_TRUE(commitBytes(m, buf, b2, sizeof(b2), false, false, 0));  // re-POST: no reset
  TEST_ASSERT_EQUAL_UINT32(5, m.version());
  TEST_ASSERT_EQUAL_size_t(2, m.imageBytes());
}

void test_ota_mgr_version_atomic_at_commit() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t a[] = {'a'};
  TEST_ASSERT_TRUE(commitBytes(m, buf, a, sizeof(a), true, true, 5));
  TEST_ASSERT_FALSE(m.beginUpload(true, true, 9));
  TEST_ASSERT_EQUAL_UINT32(5, m.version());  // label applies at commit, never ahead
  TEST_ASSERT_TRUE(m.storageReady(buf, true) == ota::StartResult::kStarted);
  TEST_ASSERT_EQUAL_UINT32(5, m.version());
  TEST_ASSERT_TRUE(m.finishUpload(true, kFakeDigest));
  TEST_ASSERT_EQUAL_UINT32(9, m.version());
}

void test_ota_mgr_bad_version_rejects_preserving() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t img[] = {'h', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_TRUE(commitBytes(m, buf, img, sizeof(img), true, true, 5));
  TEST_ASSERT_TRUE(m.beginUpload(true, false, 0));  // malformed ?version=
  TEST_ASSERT_FALSE(m.uploading());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::WriteResult::kIgnored),
                          static_cast<uint8_t>(m.writeChunk(img, sizeof(img))));
  TEST_ASSERT_FALSE(m.finishUpload(false, nullptr));
  const ota::PostResult r = m.takePostResult();
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_TRUE(r.rejected);
  TEST_ASSERT_EQUAL_UINT32(5, m.version());  // previous label preserved
  TEST_ASSERT_EQUAL_size_t(5, m.imageBytes());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(img, buf, sizeof(img));  // previous bytes preserved
}

void test_ota_mgr_overflow_resets_inflight() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t img[] = {'h', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_TRUE(commitBytes(m, buf, img, sizeof(img), true, true, 5));
  TEST_ASSERT_FALSE(m.beginUpload(false, false, 0));
  TEST_ASSERT_TRUE(m.storageReady(buf, true) == ota::StartResult::kStarted);
  const uint8_t tiny[] = {'x', 'y', 'z'};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::WriteResult::kOk),
                          static_cast<uint8_t>(m.writeChunk(tiny, sizeof(tiny))));
  // Oversize chunk: no memcpy happens (cap gate first), upload stops.
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::WriteResult::kTooBig),
                          static_cast<uint8_t>(m.writeChunk(buf, ota::kMaxImage)));
  TEST_ASSERT_FALSE(m.uploading());
  TEST_ASSERT_FALSE(m.finishUpload(false, nullptr));
  const ota::PostResult r = m.takePostResult();
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_FALSE(r.rejected);
  TEST_ASSERT_EQUAL_UINT32(5, m.version());  // committed state untouched
  TEST_ASSERT_EQUAL_size_t(5, m.imageBytes());
}

void test_ota_mgr_busy_view_during_upload() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t img[] = {'h', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_TRUE(commitBytes(m, buf, img, sizeof(img), true, true, 5));
  TEST_ASSERT_FALSE(m.beginUpload(false, false, 0));
  TEST_ASSERT_TRUE(m.storageReady(buf, true) == ota::StartResult::kStarted);
  const ota::BinView b = m.binView();
  TEST_ASSERT_TRUE(b.busy);  // busy wins over the committed image
  TEST_ASSERT_FALSE(b.hasImage);
  TEST_ASSERT_TRUE(m.finishUpload(true, kFakeDigest));  // recovers cleanly
  TEST_ASSERT_FALSE(m.binView().busy);
}

void test_ota_mgr_no_storage_stops_upload() {
  ota::StagingManager m;
  TEST_ASSERT_FALSE(m.beginUpload(false, false, 0));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::StartResult::kNoStorage),
                          static_cast<uint8_t>(m.storageReady(nullptr, false)));
  TEST_ASSERT_FALSE(m.uploading());
  TEST_ASSERT_FALSE(m.finishUpload(false, nullptr));
  const ota::PostResult r = m.takePostResult();
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_FALSE(r.rejected);
}

void test_ota_mgr_digest_fail_clears_sha() {
  ota::StagingManager m;
  uint8_t buf[256] = {};
  const uint8_t img[] = {'h', 'i'};
  TEST_ASSERT_FALSE(m.beginUpload(true, true, 3));
  TEST_ASSERT_TRUE(m.storageReady(buf, false) == ota::StartResult::kStarted);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::WriteResult::kOk),
                          static_cast<uint8_t>(m.writeChunk(img, sizeof(img))));
  m.noteDigestFailed();
  TEST_ASSERT_TRUE(m.finishUpload(false, nullptr));  // upload commits, sha clears
  TEST_ASSERT_EQUAL_UINT32(3, m.version());
  TEST_ASSERT_FALSE(m.hasSha());
  const ota::VersionView v = m.versionView();
  TEST_ASSERT_EQUAL_STRING("0000000000000000000000000000000000000000000000000000000000000000", v.sha);
  char body[otamanifest::kManifestCap];
  TEST_ASSERT_TRUE(otamanifest::buildManifest(v.version, static_cast<uint32_t>(v.len), v.sha, body, sizeof(body)));
  TEST_ASSERT_EQUAL_STRING(
      "version=3\nsize=2\nsha256=0000000000000000000000000000000000000000000000000000000000000000\n", body);
}

void test_ota_mgr_final_without_start_fails() {
  ota::StagingManager m;
  TEST_ASSERT_FALSE(m.finishUpload(false, nullptr));
  TEST_ASSERT_FALSE(m.takePostResult().ok);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::WriteResult::kIgnored),
                          static_cast<uint8_t>(m.writeChunk(nullptr, 0)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ota::WriteResult::kIgnored),
                          static_cast<uint8_t>(m.writeChunk(nullptr, 5)));
}
