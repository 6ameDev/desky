// Host unit tests for the UART POC codec + config (no hardware).
// Run: pio test -e native-test
//
// NOTE endianness: uart_frame.h is little-endian by design (ESP<->ESP only),
// a deliberate deviation from middleware/udp_codec.h (big-endian app link).

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "link/poc_config.h"
#include "link/poc_synth.h"
#include "link/uart_fec.h"
#include "link/uart_frame.h"

void test_uart_crc_known_vectors() {
  const uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_UINT16(0x29B1, uartpoc::crc16Ccitt(v, sizeof(v)));
  TEST_ASSERT_EQUAL_UINT32(0xCBF43926UL, uartpoc::crc32Ieee(v, sizeof(v)));
  TEST_ASSERT_EQUAL_UINT32(0, uartpoc::crc32Ieee(v, 0));  // Empty payload tails to zero.
}

void test_uart_roundtrip_all_types() {
  const uint8_t types[4] = {uartpoc::MSG_CMD, uartpoc::MSG_RESP, uartpoc::MSG_CHUNK, uartpoc::MSG_HB};
  uint8_t payload[32];
  for (uint8_t i = 0; i < sizeof(payload); ++i) {
    payload[i] = i;
  }
  for (uint8_t t = 0; t < 4; ++t) {
    uint8_t out[uartpoc::kMaxFrameLen];
    size_t n = 0;
    TEST_ASSERT_TRUE(uartpoc::encodeFrame(types[t], uartpoc::FLAG_SYNTHETIC, 0x0102, 0x0304, payload, sizeof(payload),
                                          out, sizeof(out), n));
    // Little-endian field order on the wire.
    TEST_ASSERT_EQUAL_UINT8(0x02, out[4]);
    TEST_ASSERT_EQUAL_UINT8(0x01, out[5]);
    TEST_ASSERT_EQUAL_UINT8(0x04, out[6]);
    TEST_ASSERT_EQUAL_UINT8(0x03, out[7]);
    TEST_ASSERT_EQUAL_UINT8(sizeof(payload), out[8]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[9]);
    uartpoc::Decoder dec;
    uartpoc::DecodedFrame fr;
    size_t consumed = 0;
    TEST_ASSERT_TRUE(dec.feed(out, n, consumed, fr) == uartpoc::DecodeStatus::OK);
    TEST_ASSERT_EQUAL_UINT((unsigned)(n), (unsigned)(consumed));
    TEST_ASSERT_EQUAL_UINT8(types[t], fr.type);
    TEST_ASSERT_EQUAL_UINT8(uartpoc::FLAG_SYNTHETIC, fr.flags);
    TEST_ASSERT_EQUAL_UINT16(0x0102, fr.frameId);
    TEST_ASSERT_EQUAL_UINT16(0x0304, fr.chunkIdx);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), fr.payloadLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, fr.payload, sizeof(payload));
  }
}

void test_uart_roundtrip_empty_and_max() {
  uint8_t out[uartpoc::kMaxFrameLen];
  size_t n = 0;
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_HB, 0, 9, 0, nullptr, 0, out, sizeof(out), n));
  TEST_ASSERT_EQUAL_UINT((unsigned)(uartpoc::kHeaderLen + uartpoc::kTailLen), (unsigned)(n));
  uint8_t big[uartpoc::kMaxPayload];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = static_cast<uint8_t>(i & 0xFF);
  }
  TEST_ASSERT_TRUE(
      uartpoc::encodeFrame(uartpoc::MSG_CHUNK, uartpoc::FLAG_LAST_CHUNK, 7, 3, big, sizeof(big), out, sizeof(out), n));
  TEST_ASSERT_EQUAL_UINT((unsigned)(uartpoc::kMaxFrameLen), (unsigned)(n));
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  size_t consumed = 0;
  TEST_ASSERT_TRUE(dec.feed(out, n, consumed, fr) == uartpoc::DecodeStatus::OK);
  TEST_ASSERT_EQUAL_UINT16(sizeof(big), fr.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(big, fr.payload, sizeof(big));
}

void test_uart_encode_rejects() {
  uint8_t out[uartpoc::kMaxFrameLen];
  uint8_t big[uartpoc::kMaxPayload + 1] = {};
  size_t n = 0;
  TEST_ASSERT_FALSE(uartpoc::encodeFrame(uartpoc::MSG_CHUNK, 0, 0, 0, big, sizeof(big), out, sizeof(out), n));
  TEST_ASSERT_FALSE(uartpoc::encodeFrame(uartpoc::MSG_CHUNK, 0, 0, 0, big, uartpoc::kMaxPayload, out, 100, n));
  TEST_ASSERT_FALSE(uartpoc::encodeFrame(uartpoc::MSG_CHUNK, 0, 0, 0, big, 10, nullptr, sizeof(out), n));
  TEST_ASSERT_FALSE(uartpoc::encodeFrame(uartpoc::MSG_CHUNK, 0, 0, 0, nullptr, 10, out, sizeof(out), n));
}

void test_uart_hdr_crc_kill() {
  uint8_t out[64];
  const uint8_t p[] = {0xAA};
  size_t n = 0;
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_CMD, 0, 1, 0, p, sizeof(p), out, sizeof(out), n));
  out[5] ^= 0xFF;  // Corrupt FrameID -> header CRC must fail.
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  size_t consumed = 0;
  TEST_ASSERT_TRUE(dec.feed(out, n, consumed, fr) == uartpoc::DecodeStatus::ERR_HDR_CRC);
}

void test_uart_pay_crc_kill() {
  uint8_t out[64];
  const uint8_t p[] = {0xAA, 0xBB};
  size_t n = 0;
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_CHUNK, 0, 1, 0, p, sizeof(p), out, sizeof(out), n));
  out[uartpoc::kHeaderLen] ^= 0xFF;  // Corrupt payload -> tail CRC must fail.
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  size_t consumed = 0;
  TEST_ASSERT_TRUE(dec.feed(out, n, consumed, fr) == uartpoc::DecodeStatus::ERR_PAY_CRC);
}

void test_uart_magic_resync() {
  uint8_t good[64];
  const uint8_t p[] = {0x44, 0x53, 0x44};  // Payload holding magic bytes must not confuse sync.
  size_t n = 0;
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_RESP, 0, 0xBEEF, 0x0007, p, sizeof(p), good, sizeof(good), n));
  uint8_t stream[96];
  size_t s = 0;
  const uint8_t garbage[] = {0x00, 0xFF, 0x44, 0x00, 0x53};  // Incl. a lone trailing 0x44 + stray 0x53.
  for (size_t i = 0; i < sizeof(garbage); ++i) {
    stream[s++] = garbage[i];
  }
  for (size_t i = 0; i < n; ++i) {
    stream[s++] = good[i];
  }
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  size_t consumed = 0;
  TEST_ASSERT_TRUE(dec.feed(stream, s, consumed, fr) == uartpoc::DecodeStatus::OK);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, fr.frameId);
  TEST_ASSERT_EQUAL_UINT16(0x0007, fr.chunkIdx);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(p, fr.payload, sizeof(p));
}

void test_uart_byte_at_a_time() {
  uint8_t out[64];
  const uint8_t p[] = {'h', 'i'};
  size_t n = 0;
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_CMD, 0, 3, 0, p, sizeof(p), out, sizeof(out), n));
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  for (size_t i = 0; i + 1 < n; ++i) {
    size_t consumed = 0;
    TEST_ASSERT_TRUE(dec.feed(out + i, 1, consumed, fr) == uartpoc::DecodeStatus::NEED_MORE);
    TEST_ASSERT_EQUAL_UINT((unsigned)(1), (unsigned)(consumed));
  }
  size_t consumed = 0;
  TEST_ASSERT_TRUE(dec.feed(out + n - 1, 1, consumed, fr) == uartpoc::DecodeStatus::OK);
  TEST_ASSERT_EQUAL_UINT16(3, fr.frameId);
}

void test_uart_overlong_dropped() {
  // Hand-built header: magic + plen 2000 (over the 1024 gate) + valid hdr CRC.
  uint8_t bad[uartpoc::kHeaderLen];
  bad[0] = 0x44;
  bad[1] = 0x53;
  bad[2] = uartpoc::MSG_CHUNK;
  bad[3] = 0;
  uartpoc::putU16LE(bad + 4, 1);
  uartpoc::putU16LE(bad + 6, 0);
  uartpoc::putU16LE(bad + 8, 2000);
  uartpoc::putU16LE(bad + 10, uartpoc::crc16Ccitt(bad, 10));
  uint8_t good[32];
  const uint8_t p[] = {0x11};
  size_t n = 0;
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_HB, 0, 0x1234, 0, p, sizeof(p), good, sizeof(good), n));
  uint8_t stream[96];
  size_t s = 0;
  for (size_t i = 0; i < sizeof(bad); ++i) {
    stream[s++] = bad[i];
  }
  for (size_t i = 0; i < n; ++i) {
    stream[s++] = good[i];
  }
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  size_t consumed = 0;
  // Overlong is a silent drop; the good frame behind it still decodes.
  TEST_ASSERT_TRUE(dec.feed(stream, s, consumed, fr) == uartpoc::DecodeStatus::OK);
  TEST_ASSERT_EQUAL_UINT16(0x1234, fr.frameId);
}

void test_uart_back_to_back() {
  uint8_t buf[128];
  size_t n1 = 0, n2 = 0;
  const uint8_t a[] = {1}, b[] = {2, 3};
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_CMD, 0, 10, 0, a, sizeof(a), buf, sizeof(buf), n1));
  TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_RESP, 0, 11, 0, b, sizeof(b), buf + n1, sizeof(buf) - n1, n2));
  uartpoc::Decoder dec;
  uartpoc::DecodedFrame fr;
  size_t consumed = 0;
  TEST_ASSERT_TRUE(dec.feed(buf, n1 + n2, consumed, fr) == uartpoc::DecodeStatus::OK);
  TEST_ASSERT_EQUAL_UINT((unsigned)(n1), (unsigned)(consumed));
  TEST_ASSERT_EQUAL_UINT16(10, fr.frameId);
  TEST_ASSERT_TRUE(dec.feed(buf + consumed, n2, consumed, fr) == uartpoc::DecodeStatus::OK);
  TEST_ASSERT_EQUAL_UINT16(11, fr.frameId);
}

static void pushChunk(uartpoc::Reassembler& r, uint16_t fid, uint16_t idx, bool last, const uint8_t* d, uint16_t len,
                      uartpoc::Reassembler::Push want, size_t wantTotal = 0) {
  size_t total = 0;
  const uint8_t flags = last ? uartpoc::FLAG_LAST_CHUNK : 0;
  TEST_ASSERT_TRUE(r.push(fid, idx, flags, d, len, total) == want);
  TEST_ASSERT_EQUAL_UINT((unsigned)(wantTotal), (unsigned)(total));
}

void test_uart_reasm_basic() {
  uint8_t slot[512];
  uartpoc::Reassembler r;
  r.attach(slot, sizeof(slot));
  const uint8_t c0[] = {0, 1, 2, 3};
  const uint8_t c1[] = {4, 5, 6, 7};
  const uint8_t c2[] = {8, 9};
  pushChunk(r, 5, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::ACCEPTED);
  pushChunk(r, 5, 1, false, c1, sizeof(c1), uartpoc::Reassembler::Push::ACCEPTED);
  pushChunk(r, 5, 2, true, c2, sizeof(c2), uartpoc::Reassembler::Push::COMPLETE, 10);
  const uint8_t expect[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, slot, sizeof(expect));
}

void test_uart_reasm_ooo_and_duplicate() {
  uint8_t slot[512];
  uartpoc::Reassembler r;
  r.attach(slot, sizeof(slot));
  const uint8_t c0[] = {0, 1};
  const uint8_t c1[] = {2, 3};
  const uint8_t c2[] = {4, 5};
  pushChunk(r, 6, 2, true, c2, sizeof(c2), uartpoc::Reassembler::Push::ACCEPTED);  // LAST-first: stashed.
  pushChunk(r, 6, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::ACCEPTED);
  pushChunk(r, 6, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::DUPLICATE);
  pushChunk(r, 6, 1, false, c1, sizeof(c1), uartpoc::Reassembler::Push::COMPLETE, 6);
  const uint8_t expect[] = {0, 1, 2, 3, 4, 5};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, slot, sizeof(expect));
}

void test_uart_reasm_loss_then_stale() {
  uint8_t slot[512];
  uartpoc::Reassembler r;
  r.attach(slot, sizeof(slot));
  const uint8_t c0[] = {0, 1};
  const uint8_t c2[] = {4, 5};
  const uint8_t n0[] = {9, 9};
  pushChunk(r, 7, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::ACCEPTED);
  // Chunk 1 lost; LAST arrives -> accepted but never completes.
  pushChunk(r, 7, 2, true, c2, sizeof(c2), uartpoc::Reassembler::Push::ACCEPTED);
  TEST_ASSERT_TRUE(r.active());
  // New frame's chunk abandons the partial (STALE); re-push starts fresh.
  size_t total = 0;
  TEST_ASSERT_TRUE(r.push(8, 0, 0, n0, sizeof(n0), total) == uartpoc::Reassembler::Push::STALE);
  pushChunk(r, 8, 0, true, n0, sizeof(n0), uartpoc::Reassembler::Push::COMPLETE, 2);
  TEST_ASSERT_EQUAL_UINT8(n0[0], slot[0]);
}

void test_uart_reasm_cap_drop() {
  uint8_t slot[6];  // Smaller than one full frame.
  uartpoc::Reassembler r;
  r.attach(slot, sizeof(slot));
  const uint8_t c0[] = {0, 1, 2, 3};
  const uint8_t c1[] = {4, 5, 6, 7};
  pushChunk(r, 9, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::ACCEPTED);
  // Cap overflow is OVERSIZE, not DROPPED (S3 64KB-slot honesty: big, not silent loss).
  pushChunk(r, 9, 1, true, c1, sizeof(c1), uartpoc::Reassembler::Push::OVERSIZE);
  TEST_ASSERT_FALSE(r.active());
}

void test_uart_reasm_oversize_vs_dropped() {
  uint8_t slot[6];
  uartpoc::Reassembler r;
  r.attach(slot, sizeof(slot));
  const uint8_t c0[] = {0, 1, 2, 3};
  const uint8_t c1[] = {4, 5, 6, 7};
  // LAST-first stash that can never resolve inside the cap -> OVERSIZE on stride resolve.
  pushChunk(r, 20, 1, true, c1, sizeof(c1), uartpoc::Reassembler::Push::ACCEPTED);
  pushChunk(r, 20, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::OVERSIZE);
  TEST_ASSERT_FALSE(r.active());
  // Every other drop reason stays DROPPED.
  uint8_t big[512];
  uartpoc::Reassembler r2;
  r2.attach(big, sizeof(big));
  size_t total = 0;
  TEST_ASSERT_TRUE(r2.push(21, uartpoc::Reassembler::kMaxChunks, 0, c0, sizeof(c0), total) ==
                   uartpoc::Reassembler::Push::DROPPED);                                          // Bad chunkIdx.
  TEST_ASSERT_TRUE(r2.push(21, 0, 0, nullptr, 4, total) == uartpoc::Reassembler::Push::DROPPED);  // Null buffer.
  TEST_ASSERT_TRUE(r2.push(21, 0, 0, c0, 0, total) == uartpoc::Reassembler::Push::DROPPED);       // Zero-len non-LAST.
  // Post-LAST excess: LAST at idx 2 recorded (chunk 1 missing, frame open),
  // then idx 3 arrives past it -> DROPPED.
  pushChunk(r2, 21, 0, false, c0, sizeof(c0), uartpoc::Reassembler::Push::ACCEPTED);
  pushChunk(r2, 21, 2, true, c1, sizeof(c1), uartpoc::Reassembler::Push::ACCEPTED);
  size_t dummy = 0;
  TEST_ASSERT_TRUE(r2.push(21, 3, 0, c0, sizeof(c0), dummy) == uartpoc::Reassembler::Push::DROPPED);
}

void test_uart_config_set_clamp() {
  uartpoc::PocConfig cfg;
  char msg[64];
  TEST_ASSERT_TRUE(uartpoc::parseSet("chunk", "4", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT16(16, cfg.chunk_bytes);
  TEST_ASSERT_EQUAL_STRING("ACK chunk 16", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("chunk", "5000", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT16(1024, cfg.chunk_bytes);
  TEST_ASSERT_TRUE(uartpoc::parseSet("pace", "99999", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT32(50000, cfg.pace_us);
  TEST_ASSERT_TRUE(uartpoc::parseSet("mode", "9", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(3, cfg.mode);
  TEST_ASSERT_TRUE(uartpoc::parseSet("fps", "0", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT16(1, cfg.fps);
  TEST_ASSERT_TRUE(uartpoc::parseSet("fps", "99", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT16(30, cfg.fps);
  TEST_ASSERT_TRUE(uartpoc::parseSet("baud", "460800", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT32(460800, cfg.baud);
  TEST_ASSERT_EQUAL_STRING("ACK baud 460800", msg);
  const uint32_t hiBauds[] = {1000000, 1500000, 2000000, 3000000, 4000000, 5000000};
  for (size_t i = 0; i < sizeof(hiBauds) / sizeof(hiBauds[0]); ++i) {
    char val[12];
    snprintf(val, sizeof(val), "%lu", static_cast<unsigned long>(hiBauds[i]));
    TEST_ASSERT_TRUE(uartpoc::parseSet("baud", val, cfg, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_UINT32(hiBauds[i], cfg.baud);
  }
  TEST_ASSERT_FALSE(uartpoc::parseSet("baud", "6000000", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("NACK bad_baud", msg);
}

void test_uart_config_reject() {
  uartpoc::PocConfig cfg;
  char msg[64];
  TEST_ASSERT_FALSE(uartpoc::parseSet("baud", "12345", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("NACK bad_baud", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("baud", "230400", cfg, msg, sizeof(msg)));  // BANNED, §3.
  TEST_ASSERT_EQUAL_STRING("NACK bad_baud", msg);
  TEST_ASSERT_EQUAL_UINT32(1500000, cfg.baud);  // Rejected SET leaves config untouched.
  TEST_ASSERT_FALSE(uartpoc::parseSet("nope", "1", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("NACK unknown_key", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("chunk", "abc", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("NACK bad_value", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("chunk", "", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("chunk", "-5", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet(nullptr, "5", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("chunk", "42949672960", cfg, msg, sizeof(msg)));  // u32 overflow.
}

void test_uart_config_get() {
  uartpoc::PocConfig cfg;
  cfg.chunk_bytes = 64;
  cfg.pace_us = 500;
  cfg.baud = 460800;
  cfg.mode = 2;
  cfg.fps = 5;
  TEST_ASSERT_EQUAL_UINT8(0, cfg.fec_k);          // Default: passthrough.
  TEST_ASSERT_EQUAL_UINT16(0, cfg.exposure);      // Default: auto AEC.
  TEST_ASSERT_EQUAL_UINT8(18, cfg.jpeg_quality);  // Default: sane mid quality.
  char out[128];
  uartpoc::formatGet(cfg, "chunk", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("chunk 64", out);
  uartpoc::formatGet(cfg, "baud", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("baud 460800", out);
  uartpoc::formatGet(cfg, "fec", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("fec 0", out);
  uartpoc::formatGet(cfg, "exposure", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("exposure 0", out);
  uartpoc::formatGet(cfg, "jpeg_quality", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("jpeg_quality 18", out);
  uartpoc::formatGet(cfg, "framesize", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("framesize qvga", out);
  uartpoc::formatGet(cfg, "all", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("chunk 64 pace 500 baud 460800 mode 2 fps 5 fec 0 exposure 0 jpeg_quality 18 framesize qvga",
                           out);
  const char* tail = strstr(out, "framesize ");
  TEST_ASSERT_NOT_NULL(tail);  // framesize stays LAST for older parsers.
  TEST_ASSERT_EQUAL_STRING("framesize qvga", tail);
  cfg.framesize = 5;
  cfg.fec_k = 2;
  cfg.exposure = 1200;
  cfg.jpeg_quality = 10;
  uartpoc::formatGet(cfg, "framesize", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("framesize uxga", out);
  uartpoc::formatGet(cfg, "all", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(
      "chunk 64 pace 500 baud 460800 mode 2 fps 5 fec 2 exposure 1200 jpeg_quality 10 framesize "
      "uxga",
      out);
  tail = strstr(out, "framesize ");
  TEST_ASSERT_NOT_NULL(tail);
  TEST_ASSERT_EQUAL_STRING("framesize uxga", tail);
  uartpoc::formatGet(cfg, "nope", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("NACK unknown_key", out);
}

void test_uart_framesize_parse() {
  TEST_ASSERT_EQUAL_INT(0, uartpoc::parseFramesize("qvga"));
  TEST_ASSERT_EQUAL_INT(1, uartpoc::parseFramesize("vga"));
  TEST_ASSERT_EQUAL_INT(2, uartpoc::parseFramesize("svga"));
  TEST_ASSERT_EQUAL_INT(3, uartpoc::parseFramesize("xga"));
  TEST_ASSERT_EQUAL_INT(4, uartpoc::parseFramesize("sxga"));
  TEST_ASSERT_EQUAL_INT(5, uartpoc::parseFramesize("uxga"));
  TEST_ASSERT_EQUAL_INT(6, uartpoc::parseFramesize("qxga"));
  TEST_ASSERT_EQUAL_INT(-1, uartpoc::parseFramesize("QVGA"));  // Case-sensitive: lowercase only.
  TEST_ASSERT_EQUAL_INT(-1, uartpoc::parseFramesize("1080p"));
  TEST_ASSERT_EQUAL_INT(-1, uartpoc::parseFramesize(""));
  TEST_ASSERT_EQUAL_INT(-1, uartpoc::parseFramesize(nullptr));
  TEST_ASSERT_EQUAL_INT(-1, uartpoc::parseFramesize("qvga "));
  // Clamp-equivalent: out-of-range indices pin to the ends.
  TEST_ASSERT_EQUAL_UINT8(0, uartpoc::clampFramesize(-1));
  TEST_ASSERT_EQUAL_UINT8(0, uartpoc::clampFramesize(-9999));
  TEST_ASSERT_EQUAL_UINT8(6, uartpoc::clampFramesize(7));
  TEST_ASSERT_EQUAL_UINT8(6, uartpoc::clampFramesize(9999));
  TEST_ASSERT_EQUAL_UINT8(0, uartpoc::clampFramesize(0));
  TEST_ASSERT_EQUAL_UINT8(3, uartpoc::clampFramesize(3));
  TEST_ASSERT_EQUAL_UINT8(6, uartpoc::clampFramesize(6));
}

void test_uart_framesize_name() {
  const char* names[7] = {"qvga", "vga", "svga", "xga", "sxga", "uxga", "qxga"};
  for (uint8_t i = 0; i < 7; ++i) {
    TEST_ASSERT_EQUAL_STRING(names[i], uartpoc::framesizeName(i));
    TEST_ASSERT_EQUAL_INT(i, uartpoc::parseFramesize(uartpoc::framesizeName(i)));  // Round-trip.
  }
  TEST_ASSERT_EQUAL_STRING("unknown", uartpoc::framesizeName(7));
  TEST_ASSERT_EQUAL_STRING("unknown", uartpoc::framesizeName(255));
}

void test_uart_config_set_framesize() {
  uartpoc::PocConfig cfg;
  char msg[64];
  TEST_ASSERT_TRUE(uartpoc::parseSet("framesize", "vga", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(1, cfg.framesize);
  TEST_ASSERT_EQUAL_STRING("ACK framesize vga", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("framesize", "qxga", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(6, cfg.framesize);
  TEST_ASSERT_EQUAL_STRING("ACK framesize qxga", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("framesize", "bogus", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("NACK bad_framesize qvga|vga|svga|xga|sxga|uxga|qxga", msg);
  TEST_ASSERT_EQUAL_UINT8(6, cfg.framesize);  // Rejected SET leaves config untouched.
  TEST_ASSERT_FALSE(uartpoc::parseSet("framesize", "", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("framesize", "1080p", cfg, msg, sizeof(msg)));
}

void test_uart_config_set_fec() {
  uartpoc::PocConfig cfg;
  TEST_ASSERT_EQUAL_UINT8(0, cfg.fec_k);  // Default: passthrough, wire-identical to task-1.
  char msg[64];
  TEST_ASSERT_TRUE(uartpoc::parseSet("fec", "2", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(2, cfg.fec_k);
  TEST_ASSERT_EQUAL_STRING("ACK fec 2", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("fec_k", "9", cfg, msg, sizeof(msg)));  // Clamp 0..4.
  TEST_ASSERT_EQUAL_UINT8(4, cfg.fec_k);
  TEST_ASSERT_EQUAL_STRING("ACK fec 4", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("fec", "0", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(0, cfg.fec_k);
  TEST_ASSERT_EQUAL_STRING("ACK fec 0", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("fec", "x", cfg, msg, sizeof(msg)));  // Non-numeric rejects.
  TEST_ASSERT_EQUAL_STRING("NACK bad_value", msg);
  TEST_ASSERT_EQUAL_UINT8(0, cfg.fec_k);  // Rejected SET leaves config untouched.
  TEST_ASSERT_FALSE(uartpoc::parseSet("fec", "", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("fec", "-1", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("fec", "42949672960", cfg, msg, sizeof(msg)));  // u32 overflow.
  char out[32];
  uartpoc::formatGet(cfg, "fec", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("fec 0", out);
  uartpoc::formatGet(cfg, "fec_k", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("fec 0", out);
}

void test_uart_config_set_exposure() {
  uartpoc::PocConfig cfg;
  TEST_ASSERT_EQUAL_UINT16(0, cfg.exposure);  // Default: auto AEC (today's behavior).
  char msg[64];
  TEST_ASSERT_TRUE(uartpoc::parseSet("exposure", "0", cfg, msg, sizeof(msg)));  // 0 stays 0 (auto).
  TEST_ASSERT_EQUAL_UINT16(0, cfg.exposure);
  TEST_ASSERT_EQUAL_STRING("ACK exposure 0", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("exposure", "300", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT16(300, cfg.exposure);
  TEST_ASSERT_EQUAL_STRING("ACK exposure 300", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("exposure", "99999", cfg, msg, sizeof(msg)));  // Clamp to the 1200-line ceiling.
  TEST_ASSERT_EQUAL_UINT16(1200, cfg.exposure);
  TEST_ASSERT_EQUAL_STRING("ACK exposure 1200", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("exposure", "x", cfg, msg, sizeof(msg)));  // Non-numeric rejects.
  TEST_ASSERT_EQUAL_STRING("NACK bad_value", msg);
  TEST_ASSERT_EQUAL_UINT16(1200, cfg.exposure);  // Rejected SET leaves config untouched.
  TEST_ASSERT_FALSE(uartpoc::parseSet("exposure", "", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("exposure", "-1", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("exposure", "42949672960", cfg, msg, sizeof(msg)));  // u32 overflow.
  char out[32];
  uartpoc::formatGet(cfg, "exposure", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("exposure 1200", out);
}

void test_uart_config_set_quality() {
  uartpoc::PocConfig cfg;
  TEST_ASSERT_EQUAL_UINT8(18, cfg.jpeg_quality);  // Default: sane mid quality.
  char msg[64];
  TEST_ASSERT_TRUE(uartpoc::parseSet("jpeg_quality", "16", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(16, cfg.jpeg_quality);
  TEST_ASSERT_EQUAL_STRING("ACK jpeg_quality 16", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("jpeg_quality", "9", cfg, msg, sizeof(msg)));  // Clamp 10..30.
  TEST_ASSERT_EQUAL_UINT8(10, cfg.jpeg_quality);
  TEST_ASSERT_EQUAL_STRING("ACK jpeg_quality 10", msg);
  TEST_ASSERT_TRUE(uartpoc::parseSet("jpeg_quality", "99", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT8(30, cfg.jpeg_quality);
  TEST_ASSERT_EQUAL_STRING("ACK jpeg_quality 30", msg);
  TEST_ASSERT_FALSE(uartpoc::parseSet("jpeg_quality", "x", cfg, msg, sizeof(msg)));  // Non-numeric rejects.
  TEST_ASSERT_EQUAL_STRING("NACK bad_value", msg);
  TEST_ASSERT_EQUAL_UINT8(30, cfg.jpeg_quality);  // Rejected SET leaves config untouched.
  TEST_ASSERT_FALSE(uartpoc::parseSet("jpeg_quality", "", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("jpeg_quality", "-1", cfg, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(uartpoc::parseSet("jpeg_quality", "42949672960", cfg, msg, sizeof(msg)));  // u32 overflow.
  char out[32];
  uartpoc::formatGet(cfg, "jpeg_quality", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("jpeg_quality 30", out);
}

void test_fec_emit_plan() {
  uartpoc::fec::EmitPlan p = uartpoc::fec::planEmit(0, 8, 128);  // K=0 default: passthrough.
  TEST_ASSERT_FALSE(p.emit);
  TEST_ASSERT_EQUAL_UINT8(0, p.k);
  p = uartpoc::fec::planEmit(2, 8, 128);  // Normal synth-sized frame emits.
  TEST_ASSERT_TRUE(p.emit);
  TEST_ASSERT_EQUAL_UINT8(2, p.k);
  p = uartpoc::fec::planEmit(9, 8, 128);  // Defensive clamp at the emit edge (SET already clamps).
  TEST_ASSERT_TRUE(p.emit);
  TEST_ASSERT_EQUAL_UINT8(4, p.k);
  TEST_ASSERT_FALSE(uartpoc::fec::planEmit(2, 0, 128).emit);    // Empty frame.
  TEST_ASSERT_FALSE(uartpoc::fec::planEmit(2, 65, 128).emit);   // N > kMaxData.
  TEST_ASSERT_TRUE(uartpoc::fec::planEmit(2, 64, 128).emit);    // N == kMaxData edge.
  TEST_ASSERT_FALSE(uartpoc::fec::planEmit(2, 8, 0).emit);      // Bad stride.
  TEST_ASSERT_FALSE(uartpoc::fec::planEmit(2, 8, 1025).emit);   // Over the codec ceiling.
  TEST_ASSERT_TRUE(uartpoc::fec::planEmit(2, 16, 1024).emit);   // 16*1024 = cap: emits.
  TEST_ASSERT_FALSE(uartpoc::fec::planEmit(2, 17, 1024).emit);  // 17*1024 > cap: skips.
}

void test_fec_parity_headers() {
  // Parity header contract: flags exactly IS_PARITY (never LAST_CHUNK),
  // chunkIdx N..N+K-1, payloadLen the full stride.
  TEST_ASSERT_EQUAL_UINT8(0x04, uartpoc::fec::kFlagParity);
  TEST_ASSERT_EQUAL_UINT8(0x04, uartpoc::fec::parityChunkFlags());
  TEST_ASSERT_EQUAL_UINT8(0, uartpoc::fec::parityChunkFlags() & uartpoc::FLAG_LAST_CHUNK);
  TEST_ASSERT_EQUAL_UINT16(8, uartpoc::fec::parityChunkIdx(8, 0));
  TEST_ASSERT_EQUAL_UINT16(10, uartpoc::fec::parityChunkIdx(8, 2));
  TEST_ASSERT_EQUAL_UINT16(66, uartpoc::fec::parityChunkIdx(64, 2));
}

void test_fec_emit_shape_roundtrip() {
  // Mirrors the Head emit path policy: N data chunks (LAST short) -> K
  // full-stride parity chunks via the emit plan -> lose one data chunk ->
  // recover (LAST truncates; encode() owned the padding).
  const uint8_t mode = 1;
  const uint16_t stride = 64;
  const uint32_t total = pocself::synthTotal(mode);  // 1024.
  const uint32_t n = (total + stride - 1) / stride;  // 16.
  TEST_ASSERT_EQUAL_UINT32(16, n);
  const uartpoc::fec::EmitPlan plan = uartpoc::fec::planEmit(2, n, stride);
  TEST_ASSERT_TRUE(plan.emit);
  TEST_ASSERT_EQUAL_UINT8(2, plan.k);
  uint8_t data[16][64];
  uint8_t par[2][64];
  const uint8_t* dptr[16];
  uint8_t* pptr[2] = {par[0], par[1]};
  size_t lens[16];
  const uint16_t fid = 7;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t off = i * stride;
    uint16_t ln = stride;
    if (off + ln > total) {
      ln = static_cast<uint16_t>(total - off);
    }
    pocself::fillSynthetic(mode, fid, off, data[i], ln, total);
    dptr[i] = data[i];
    lens[i] = ln;
  }
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, static_cast<uint8_t>(n), plan.k, stride, lens, pptr));
  for (uint8_t j = 0; j < plan.k; ++j) {
    TEST_ASSERT_EQUAL_UINT8(uartpoc::fec::kFlagParity, uartpoc::fec::parityChunkFlags());
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(n + j), uartpoc::fec::parityChunkIdx(n, j));
  }
  uint8_t keep[64];
  memcpy(keep, data[3], stride);
  memset(data[3], 0, stride);  // Lose data chunk 3.
  uint8_t* ioptr[16];
  for (uint32_t i = 0; i < n; ++i) {
    ioptr[i] = data[i];
  }
  const uint8_t* pcptr[2] = {par[0], par[1]};
  bool missing[16] = {};
  missing[3] = true;
  const bool pok[2] = {true, true};
  uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
  TEST_ASSERT_TRUE(uartpoc::fec::recover(ioptr, missing, static_cast<uint8_t>(n), pcptr, pok, plan.k, stride,
                                         total - (n - 1) * stride, scratch,
                                         sizeof(scratch)) == uartpoc::fec::Recover::OK);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(keep, data[3], stride);
}

void test_synth_totals_and_flags() {
  TEST_ASSERT_EQUAL_UINT32(512, pocself::synthTotal(0));
  TEST_ASSERT_EQUAL_UINT32(1024, pocself::synthTotal(1));
  TEST_ASSERT_EQUAL_UINT32(2048, pocself::synthTotal(2));
  TEST_ASSERT_EQUAL_UINT32(0, pocself::synthTotal(3));
  TEST_ASSERT_EQUAL_UINT8(0, pocself::synthFlags(0));
  TEST_ASSERT_EQUAL_UINT8(uartpoc::FLAG_SYNTHETIC, pocself::synthFlags(1));
  TEST_ASSERT_EQUAL_UINT8(uartpoc::FLAG_SYNTHETIC, pocself::synthFlags(2));
  TEST_ASSERT_EQUAL_UINT8(0, pocself::synthFlags(3));
}

void test_synth_ramp_pattern() {
  uint8_t a[16];
  pocself::fillSynthetic(1, 7, 250, a, sizeof(a), pocself::synthTotal(1));
  for (size_t i = 0; i < sizeof(a); ++i) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>((250 + i) & 0xFF), a[i]);
  }
  uint8_t b[16];
  pocself::fillSynthetic(1, 7, 250, b, sizeof(b), pocself::synthTotal(1));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, sizeof(a));  // Deterministic in (fid, off).
}

void test_synth_jpeg_markers_and_determinism() {
  const uint32_t total = pocself::synthTotal(2);
  uint8_t first[64];
  pocself::fillSynthetic(2, 3, 0, first, sizeof(first), total);
  TEST_ASSERT_EQUAL_UINT8(0xFF, first[0]);
  TEST_ASSERT_EQUAL_UINT8(0xD8, first[1]);
  uint8_t last[64];
  pocself::fillSynthetic(2, 3, total - sizeof(last), last, sizeof(last), total);
  TEST_ASSERT_EQUAL_UINT8(0xFF, last[sizeof(last) - 2]);
  TEST_ASSERT_EQUAL_UINT8(0xD9, last[sizeof(last) - 1]);
  uint8_t rep[64];
  pocself::fillSynthetic(2, 3, 0, rep, sizeof(rep), total);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first, rep, sizeof(first));
  uint8_t other[64];
  pocself::fillSynthetic(2, 4, 0, other, sizeof(other), total);
  TEST_ASSERT_EQUAL_UINT8(first[0], other[0]);                              // SOI marker is fid-independent...
  TEST_ASSERT_FALSE(memcmp(first + 2, other + 2, sizeof(first) - 2) == 0);  // ...but payload is fid-seeded.
}

void test_synth_text_shape() {
  uint8_t line[32];
  pocself::fillSynthetic(0, 5, 0, line, sizeof(line), pocself::synthTotal(0));
  const char* expect = "TXT 0005:0000 ";
  for (size_t i = 0; expect[i] != '\0'; ++i) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expect[i]), line[i]);
  }
  TEST_ASSERT_EQUAL_UINT8('\n', line[31]);
  uint8_t line2[32];
  pocself::fillSynthetic(0, 5, 32, line2, sizeof(line2), pocself::synthTotal(0));
  const char* expect2 = "TXT 0005:0032 ";
  for (size_t i = 0; expect2[i] != '\0'; ++i) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expect2[i]), line2[i]);
  }
  uint8_t rep[32];
  pocself::fillSynthetic(0, 5, 0, rep, sizeof(rep), pocself::synthTotal(0));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(line, rep, sizeof(line));
}

void test_synth_null_safe() {
  pocself::fillSynthetic(0, 1, 0, nullptr, 10, pocself::synthTotal(0));
  pocself::fillSynthetic(1, 1, 0, nullptr, 10, pocself::synthTotal(1));
  pocself::fillSynthetic(2, 1, 0, nullptr, 10, pocself::synthTotal(2));
}

void test_synth_mem_loopback_modes012() {
  // Mirrors the firmware SELFTEST MEM path: fragment -> encode -> 7-byte
  // slices -> Decoder -> Reassembler -> regen + memcmp.
  for (uint8_t mode = 0; mode <= 2; ++mode) {
    const uint32_t total = pocself::synthTotal(mode);
    const uint16_t stride = 64;
    const uint32_t nChunks = (total + stride - 1) / stride;
    uint8_t slot[2048];
    uartpoc::Reassembler reasm;
    reasm.attach(slot, sizeof(slot));
    uartpoc::Decoder dec;
    uint8_t src[uartpoc::kMaxPayload];
    uint8_t exp[uartpoc::kMaxPayload];
    uint8_t enc[uartpoc::kMaxFrameLen];
    uint32_t herr = 0, perr = 0, drops = 0, completes = 0;
    const uint16_t fid = 42;
    for (uint32_t idx = 0; idx < nChunks; ++idx) {
      const uint32_t off = idx * stride;
      uint16_t n = stride;
      if (off + n > total) {
        n = static_cast<uint16_t>(total - off);
      }
      pocself::fillSynthetic(mode, fid, off, src, n, total);
      uint8_t fl = pocself::synthFlags(mode);
      if (idx + 1 >= nChunks) {
        fl |= uartpoc::FLAG_LAST_CHUNK;
      }
      size_t encLen = 0;
      TEST_ASSERT_TRUE(uartpoc::encodeFrame(uartpoc::MSG_CHUNK, fl, fid, static_cast<uint16_t>(idx), src, n, enc,
                                            sizeof(enc), encLen));
      size_t pos = 0;
      while (pos < encLen) {
        size_t sl = encLen - pos;
        if (sl > pocself::kSelftestSlice) {
          sl = pocself::kSelftestSlice;
        }
        size_t o = 0;
        while (o < sl) {
          size_t consumed = 0;
          uartpoc::DecodedFrame fr;
          const uartpoc::DecodeStatus st = dec.feed(enc + pos + o, sl - o, consumed, fr);
          o += consumed;
          if (st == uartpoc::DecodeStatus::OK) {
            size_t tot = 0;
            uartpoc::Reassembler::Push pr =
                reasm.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, tot);
            if (pr == uartpoc::Reassembler::Push::STALE) {
              ++drops;
              pr = reasm.push(fr.frameId, fr.chunkIdx, fr.flags, fr.payload, fr.payloadLen, tot);
            }
            TEST_ASSERT_TRUE(pr == uartpoc::Reassembler::Push::ACCEPTED || pr == uartpoc::Reassembler::Push::COMPLETE);
            pocself::fillSynthetic(mode, fr.frameId, off, exp, n, total);
            TEST_ASSERT_EQUAL_UINT16(n, fr.payloadLen);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, fr.payload, n);
            if (pr == uartpoc::Reassembler::Push::COMPLETE) {
              TEST_ASSERT_EQUAL_UINT((unsigned)total, (unsigned)tot);
              ++completes;
            }
          } else if (st == uartpoc::DecodeStatus::ERR_HDR_CRC) {
            ++herr;
          } else if (st == uartpoc::DecodeStatus::ERR_PAY_CRC) {
            ++perr;
          } else {
            break;
          }
          if (consumed == 0) {
            break;
          }
        }
        pos += sl;
      }
    }
    TEST_ASSERT_EQUAL_UINT32(0, herr);
    TEST_ASSERT_EQUAL_UINT32(0, perr);
    TEST_ASSERT_EQUAL_UINT32(0, drops);
    TEST_ASSERT_EQUAL_UINT32(1, completes);
  }
}

void test_uart_reasm_stale_due() {
  // Inactive never due, regardless of clock values.
  TEST_ASSERT_FALSE(uartpoc::reasmStaleDue(false, 0, 100000, 100));
  TEST_ASSERT_FALSE(uartpoc::reasmStaleDue(false, 5000, 5000, 100));
  // Fresh partial: elapsed < timeout.
  TEST_ASSERT_FALSE(uartpoc::reasmStaleDue(true, 1000, 1050, 100));
  TEST_ASSERT_FALSE(uartpoc::reasmStaleDue(true, 1000, 1099, 100));
  // Boundary is due (>=).
  TEST_ASSERT_TRUE(uartpoc::reasmStaleDue(true, 1000, 1100, 100));
  TEST_ASSERT_TRUE(uartpoc::reasmStaleDue(true, 1000, 5000, 100));
  // Zero timeout: any active partial is immediately due.
  TEST_ASSERT_TRUE(uartpoc::reasmStaleDue(true, 1000, 1000, 0));
  // millis() wrap: last=0xFFFFFFF0, now=0x0000000F (31ms later) < 100ms.
  TEST_ASSERT_FALSE(uartpoc::reasmStaleDue(true, 0xFFFFFFF0UL, 0x0000000FUL, 100));
  // Same wrap, 131ms later > 100ms: due.
  TEST_ASSERT_TRUE(uartpoc::reasmStaleDue(true, 0xFFFFFFF0UL, 0x00000073UL, 100));
}

// ── S3 FEC-RX policy mirror (recover-before-reset) ──
// Host mirror of the PocLink FEC-RX policy in core/src/middleware/poc_link.h:
// parity intercepted before Reassembler (keyed by the active frameId, never
// in reassembly accounting); recovery when LAST is known with
// 1<=missing<=good parity, reconstructions re-pushed through the normal path
// (COMPLETE fires honestly with the right total); rec-vs-clean counters;
// STALE flushes parity (no cross-frame leakage). Stack storage stands in for
// the firmware INTERNAL buffers; test strides never exceed kPRow.
namespace s3fec {

struct Rx {
  static const size_t kPRow = 64;  // Parity staging row width (test stride cap).
  uint8_t* slot;
  size_t cap;
  uint8_t* pbuf;  // 4 x kPRow parity staging (caller storage).
  uartpoc::Reassembler r;
  bool act;
  uint16_t fid;
  size_t stride;
  bool haveLast;
  uint16_t lastIdx;
  uint16_t lastLen;
  bool pend;
  uint16_t pendIdx;
  uint32_t bits[32];
  bool phave;
  uint16_t pfid;
  uint8_t pn;
  uint16_t pidx[4];
  uint16_t plen[4];
  uint32_t ok;      // Clean completions (framesOk_).
  uint32_t rec;     // Recovery completions (recFec_).
  uint32_t drops;   // STALE/DROPPED/ageout path.
  uint32_t big;     // OVERSIZE subset of drops.
  uint32_t bytes;   // Payload totals, clean + recovered identically.
  uint32_t chunks;  // DATA chunks only (mirrors chunksRx_); parity never touches it.
  uint32_t prx;     // Accepted parity stores (mirrors parRx_); ignored parity stays silent.

  void attach(uint8_t* s, size_t c, uint8_t* pb) {
    slot = s;
    cap = c;
    pbuf = pb;
    r.attach(s, c);
    reset();
  }

  void reset() {
    act = false;
    fid = 0;
    stride = 0;
    haveLast = false;
    lastIdx = 0;
    lastLen = 0;
    pend = false;
    pendIdx = 0;
    for (size_t i = 0; i < uartpoc::Reassembler::kBitmapWords; ++i) {
      bits[i] = 0;
    }
    phave = false;
    pfid = 0;
    pn = 0;
    for (uint8_t i = 0; i < 4; ++i) {
      pidx[i] = 0;
      plen[i] = 0;
    }
    ok = rec = drops = big = bytes = chunks = prx = 0;
    r.reset();
  }

  void parReset() {
    phave = false;
    pfid = 0;
    pn = 0;
    for (uint8_t i = 0; i < 4; ++i) {
      pidx[i] = 0;
      plen[i] = 0;
    }
  }

  void dReset() {
    act = false;
    fid = 0;
    stride = 0;
    haveLast = false;
    lastIdx = 0;
    lastLen = 0;
    pend = false;
    pendIdx = 0;
    for (size_t i = 0; i < uartpoc::Reassembler::kBitmapWords; ++i) {
      bits[i] = 0;
    }
  }

  void setBit(uint16_t idx) { bits[idx / 32] |= (1UL << (idx % 32)); }

  bool isSet(uint16_t idx) const { return (bits[idx / 32] & (1UL << (idx % 32))) != 0; }

  // Single entry mirroring PocLink::onFrame_ routing: IS_PARITY chunks
  // intercept before reassembly, data flows to the Reassembler.
  void feed(uint16_t f, uint16_t idx, uint8_t flags, const uint8_t* p, uint16_t len) {
    if ((flags & uartpoc::fec::kFlagParity) != 0) {
      feedParity(f, idx, p, len);
      return;
    }
    ++chunks;  // DATA only (mirrors S3 chunksRx_): parity intercepts above, never counts.
    size_t total = 0;
    uartpoc::Reassembler::Push res = r.push(f, idx, flags, p, len, total);
    if (res == uartpoc::Reassembler::Push::STALE) {
      ++drops;
      parReset();
      dReset();
      res = r.push(f, idx, flags, p, len, total);
    }
    sync(f);
    if (res == uartpoc::Reassembler::Push::COMPLETE) {
      ++ok;
      bytes += static_cast<uint32_t>(total);
      parReset();
    } else if (res == uartpoc::Reassembler::Push::OVERSIZE) {
      ++drops;
      ++big;
      parReset();
    } else if (res == uartpoc::Reassembler::Push::DROPPED) {
      ++drops;
    } else if (res != uartpoc::Reassembler::Push::DUPLICATE) {  // ACCEPTED.
      note(idx, flags, len);
      recover();
    }
  }

  void feedParity(uint16_t f, uint16_t idx, const uint8_t* p, uint16_t len) {
    if (!act || f != fid) {
      return;  // Non-active/older/post-done frame: ignore, no counter.
    }
    if (len == 0 || len > kPRow || p == nullptr) {
      return;
    }
    if (!phave || pfid != f) {
      parReset();
      pfid = f;
      phave = true;
    }
    if (pn >= 4) {
      return;
    }
    for (uint8_t i = 0; i < pn; ++i) {
      if (pidx[i] == idx) {
        return;
      }
    }
    for (uint16_t i = 0; i < len; ++i) {
      pbuf[static_cast<size_t>(pn) * kPRow + i] = p[i];
    }
    pidx[pn] = idx;
    plen[pn] = len;
    ++pn;
    ++prx;  // Accepted store only (mirrors S3 parRx_): duplicates/foreign/empty return above.
    recover();
  }

  void stale() {  // Mirrors staleFlush_ firing.
    if (!r.active()) {
      return;
    }
    parReset();
    dReset();
    r.reset();
    ++drops;
  }

  void sync(uint16_t f) {
    if (!r.active()) {
      dReset();
      return;
    }
    if (!act || f != fid) {
      if (phave && pfid != f) {
        parReset();
      }
      dReset();
      act = true;
      fid = f;
    }
  }

  void note(uint16_t idx, uint8_t flags, uint16_t len) {
    const bool last = (flags & uartpoc::FLAG_LAST_CHUNK) != 0;
    if (!last && stride == 0 && len > 0) {
      stride = len;
      if (pend) {
        setBit(pendIdx);
        pend = false;
      }
    }
    if (last && idx == 0 && stride == 0) {
      stride = (len > 0) ? len : 1;
    }
    if (last && stride == 0 && idx != 0) {
      haveLast = true;
      lastIdx = idx;
      lastLen = len;
      pend = true;
      pendIdx = idx;
      return;
    }
    setBit(idx);
    if (last) {
      haveLast = true;
      lastIdx = idx;
      lastLen = len;
    }
  }

  void recover() {
    if (!act || !haveLast || pend || stride == 0) {
      return;
    }
    if (!phave || pfid != fid || pn == 0) {
      return;
    }
    if (slot == nullptr) {
      return;
    }
    const uint32_t n = static_cast<uint32_t>(lastIdx) + 1;
    if (n > uartpoc::fec::kMaxData) {
      return;
    }
    if (stride == 0 || stride > uartpoc::kMaxPayload) {
      return;
    }
    if (n * stride > cap) {
      return;  // Would OVERSIZE: the normal path flags big.
    }
    if (lastLen > stride) {
      return;
    }
    uint8_t missIdx[uartpoc::fec::kMaxData];
    uint8_t nMiss = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (!isSet(static_cast<uint16_t>(i))) {
        missIdx[nMiss++] = static_cast<uint8_t>(i);
      }
    }
    if (nMiss == 0) {
      return;
    }
    const uint8_t* parPtr[uartpoc::fec::kMaxParity] = {nullptr, nullptr, nullptr, nullptr};
    bool parOk[uartpoc::fec::kMaxParity] = {false, false, false, false};
    uint8_t nGood = 0;
    for (uint8_t s = 0; s < pn; ++s) {
      if (plen[s] != stride) {
        continue;
      }
      if (static_cast<uint32_t>(pidx[s]) < n) {
        continue;
      }
      const uint32_t j = static_cast<uint32_t>(pidx[s]) - n;
      if (j >= uartpoc::fec::kMaxParity || parOk[j]) {
        continue;
      }
      parPtr[j] = pbuf + static_cast<size_t>(s) * kPRow;
      parOk[j] = true;
      ++nGood;
    }
    if (nMiss > nGood) {
      return;  // NEED_MORE: wait for the remaining parity.
    }
    uint8_t* dataIo[uartpoc::fec::kMaxData];
    bool missing[uartpoc::fec::kMaxData];
    for (uint32_t i = 0; i < n; ++i) {
      dataIo[i] = slot + i * stride;
      missing[i] = !isSet(static_cast<uint16_t>(i));
    }
    for (size_t b = lastLen; b < stride; ++b) {
      dataIo[n - 1][b] = 0;  // Stride-padding rule: normalize the short-LAST tail.
    }
    uint8_t scratch[uartpoc::fec::kRecoverScratchMin] = {};
    const uartpoc::fec::Recover rc =
        uartpoc::fec::recover(dataIo, missing, static_cast<uint8_t>(n), parPtr, parOk, uartpoc::fec::kMaxParity, stride,
                              lastLen, scratch, sizeof(scratch));
    if (rc != uartpoc::fec::Recover::OK) {
      return;  // UNRECOVERABLE: never emit, the drop path owns it.
    }
    for (uint8_t m = 0; m < nMiss; ++m) {
      const uint8_t idx = missIdx[m];
      const bool isLast = (idx == lastIdx);
      const uint16_t ln = isLast ? lastLen : static_cast<uint16_t>(stride);
      const uint8_t fl = isLast ? uartpoc::FLAG_LAST_CHUNK : 0;
      size_t t2 = 0;
      const uartpoc::Reassembler::Push r2 = r.push(fid, idx, fl, slot + static_cast<size_t>(idx) * stride, ln, t2);
      if (r2 == uartpoc::Reassembler::Push::COMPLETE) {
        ++rec;
        bytes += static_cast<uint32_t>(t2);
        parReset();
        dReset();
        return;
      }
      if (r2 == uartpoc::Reassembler::Push::ACCEPTED) {
        setBit(idx);
        continue;
      }
      if (r2 == uartpoc::Reassembler::Push::DUPLICATE) {
        continue;
      }
      act = r.active();
      if (!act) {
        dReset();
      }
      return;
    }
  }
};

void fillData(uint8_t n, size_t stride, size_t lastLen, uint8_t data[8][64], uint32_t seed) {
  for (uint8_t i = 0; i < n; ++i) {
    const size_t ln = (i + 1 == n) ? lastLen : stride;
    for (size_t b = 0; b < ln; ++b) {
      const uint32_t x = seed + static_cast<uint32_t>(i) * 1315423911U + static_cast<uint32_t>(b) * 97U;
      data[i][b] = static_cast<uint8_t>(((x ^ (x >> 13) ^ (x << 7)) & 0xFFU));
    }
  }
}

void makeParity(uint8_t n, size_t stride, size_t lastLen, uint8_t data[8][64], uint8_t k, uint8_t par[4][64]) {
  const uint8_t* dptr[8];
  uint8_t* pptr[4];
  size_t lens[8];
  for (uint8_t i = 0; i < n; ++i) {
    dptr[i] = data[i];
    lens[i] = (i + 1 == n) ? lastLen : stride;
  }
  for (uint8_t j = 0; j < k; ++j) {
    pptr[j] = par[j];
  }
  TEST_ASSERT_TRUE(uartpoc::fec::encode(dptr, n, k, stride, lens, pptr));
}

// STATS order contract mirrors: the firmware builders are ARDUINO-guarded
// (S3: core/src/middleware/poc_link.h buildStats_; Head:
// head/src/services/poc_manager.h buildStats_), so host tests pin the token
// order here — these helpers must stay byte-identical to the firmware format
// strings, and any firmware reorder must update them (and the tests below):
//   S3:   "... up=<ms> rec=<n> par=<n>" — rec then par appended LAST.
//   Head: "... intfree=<n> txp=<n> txdrop=<n> pumpMs=<n> grabMs=<n>" — grabMs appended LAST.
void formatS3Stats(char* out, size_t cap, uint32_t ok, uint32_t chunks, uint32_t herr, uint32_t perr, uint32_t drops,
                   uint32_t big, uint32_t ooo, uint32_t dups, uint32_t ovf, uint32_t hb, uint32_t bytes, uint32_t kbps,
                   uint32_t kbs, uint32_t intfree, uint32_t up, uint32_t rec, uint32_t prx) {
  snprintf(out, cap,
           "STATS ok=%lu chunks=%lu herr=%lu perr=%lu drops=%lu big=%lu ooo=%lu dups=%lu ovf=%lu hb=%lu bytes=%lu "
           "kbps=%lu kbs=%lu intfree=%u up=%lu rec=%lu par=%lu",
           static_cast<unsigned long>(ok), static_cast<unsigned long>(chunks), static_cast<unsigned long>(herr),
           static_cast<unsigned long>(perr), static_cast<unsigned long>(drops), static_cast<unsigned long>(big),
           static_cast<unsigned long>(ooo), static_cast<unsigned long>(dups), static_cast<unsigned long>(ovf),
           static_cast<unsigned long>(hb), static_cast<unsigned long>(bytes), static_cast<unsigned long>(kbps),
           static_cast<unsigned long>(kbs), static_cast<unsigned>(intfree), static_cast<unsigned long>(up),
           static_cast<unsigned long>(rec), static_cast<unsigned long>(prx));
}

void formatHeadStats(char* out, size_t cap, uint32_t txf, uint32_t txc, uint32_t txb, uint32_t rxcmd, uint32_t rxe,
                     uint32_t mode, uint32_t chunk, uint32_t pace, uint32_t baud, uint32_t fps, uint32_t intfree,
                     uint32_t txp, uint32_t txdrop, uint32_t pumpMs, uint32_t grabMs) {
  snprintf(out, cap,
           "STATS txf=%lu txc=%lu txb=%lu rxcmd=%lu rxe=%lu mode=%u chunk=%u pace=%lu baud=%lu fps=%u intfree=%u "
           "txp=%lu txdrop=%lu pumpMs=%lu grabMs=%lu",
           static_cast<unsigned long>(txf), static_cast<unsigned long>(txc), static_cast<unsigned long>(txb),
           static_cast<unsigned long>(rxcmd), static_cast<unsigned long>(rxe), static_cast<unsigned>(mode),
           static_cast<unsigned>(chunk), static_cast<unsigned long>(pace), static_cast<unsigned long>(baud),
           static_cast<unsigned>(fps), static_cast<unsigned>(intfree), static_cast<unsigned long>(txp),
           static_cast<unsigned long>(txdrop), static_cast<unsigned long>(pumpMs), static_cast<unsigned long>(grabMs));
}

}  // namespace s3fec

void test_s3fec_recover_one_missing() {
  // N=5, short LAST present, middle chunk lost, dirty slot: recover-1
  // completes via rec (not ok) with exact total/bytes and exact payloads.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  for (size_t i = 0; i < sizeof(slot); ++i) {
    slot[i] = 0xA5;  // Stale bytes: the padding rule must still hold.
  }
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 5;
  const size_t kS = 32;
  const size_t kLast = 12;
  const uint16_t kFid = 41;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kLast, data, 0x51);
  s3fec::makeParity(kN, kS, kLast, data, 2, par);
  for (uint8_t i = 0; i < kN; ++i) {
    if (i == 2) {
      continue;  // Lost on the wire.
    }
    const bool last = (i + 1 == kN);
    rx.feed(kFid, i, last ? uartpoc::FLAG_LAST_CHUNK : 0, data[i], static_cast<uint16_t>(last ? kLast : kS));
  }
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // No parity yet: nothing solved.
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);  // Recovered, not clean.
  TEST_ASSERT_EQUAL_UINT32(1, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>((kN - 1) * kS + kLast), rx.bytes);
  for (uint8_t i = 0; i < kN; ++i) {
    const size_t ln = (i + 1 == kN) ? kLast : kS;
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data[i], slot + static_cast<size_t>(i) * kS, ln);
  }
  TEST_ASSERT_EQUAL_UINT8(0, slot[(kN - 1) * kS + kLast]);  // Padding solves to encode-time zeros.
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1],
          static_cast<uint16_t>(kS));  // Late: ignored.
  TEST_ASSERT_EQUAL_UINT32(1, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(0, rx.drops);
}

void test_s3fec_recover_two_missing_k2() {
  // 2 missing with K=2: first parity waits (NEED_MORE), second completes.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 6;
  const size_t kS = 16;
  const uint16_t kFid = 42;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0x77);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  for (uint8_t i = 0; i < kN; ++i) {
    if (i == 1 || i == 4) {
      continue;  // Lost burst.
    }
    const bool last = (i + 1 == kN);
    rx.feed(kFid, i, last ? uartpoc::FLAG_LAST_CHUNK : 0, data[i], static_cast<uint16_t>(kS));
  }
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // 2 missing > 1 good: waits.
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(1, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kN * kS), rx.bytes);
  for (uint8_t i = 0; i < kN; ++i) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data[i], slot + static_cast<size_t>(i) * kS, kS);
  }
}

void test_s3fec_kplus1_missing_drops() {
  // K+1 missing: no recovery attempted; the partial dies the existing drop path.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 5;
  const size_t kS = 32;
  const uint16_t kFid = 43;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0x99);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  for (uint8_t i = 3; i < kN; ++i) {
    const bool last = (i + 1 == kN);
    rx.feed(kFid, i, last ? uartpoc::FLAG_LAST_CHUNK : 0, data[i], static_cast<uint16_t>(kS));
  }
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // 3 missing > K=2: untouched.
  rx.stale();
  TEST_ASSERT_EQUAL_UINT32(1, rx.drops);
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
}

void test_s3fec_parity_unknown_frame_ignored() {
  // Parity for a non-active/older frameId is ignored with no counter.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  uint8_t junk[16] = {};
  rx.feed(9, 3, uartpoc::fec::kFlagParity, junk, sizeof(junk));  // No active frame.
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);
  const uint8_t kN = 3;
  const size_t kS = 16;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0xAB);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  rx.feed(7, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(9, 3, uartpoc::fec::kFlagParity, junk, sizeof(junk));  // Older frameId while 7 active.
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);
  rx.feed(7, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));  // Live frame: buffered.
  TEST_ASSERT_EQUAL_UINT8(1, rx.pn);
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(0, rx.drops);
}

void test_s3fec_clean_complete_discards_parity() {
  // Zero missing with parity buffered: COMPLETE clean, parity dropped silently.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 4;
  const size_t kS = 16;
  const size_t kLast = 10;
  const uint16_t kFid = 44;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kLast, data, 0xCD);
  s3fec::makeParity(kN, kS, kLast, data, 2, par);
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, 1, 0, data[1], static_cast<uint16_t>(kS));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);  // LAST not yet seen.
  rx.feed(kFid, 2, 0, data[2], static_cast<uint16_t>(kS));
  rx.feed(kFid, 3, uartpoc::FLAG_LAST_CHUNK, data[3], static_cast<uint16_t>(kLast));
  TEST_ASSERT_EQUAL_UINT32(1, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // Clean, not recovered.
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>((kN - 1) * kS + kLast), rx.bytes);
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);  // Buffered parity discarded silently.
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));  // Late: ignored.
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);
  TEST_ASSERT_EQUAL_UINT32(1, rx.ok);
}

void test_s3fec_last_missing_truncation() {
  // A never-arriving LAST leaves N unknowable: no recovery attempted. With
  // LAST present and a middle chunk lost, recovery solves full-stride and the
  // re-push of N-1 truncates to last_len exactly.
  uint8_t slot[1024];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 4;
  const size_t kS = 48;
  const size_t kLast = 17;
  const uint16_t kFid = 45;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kLast, data, 0xEF);
  s3fec::makeParity(kN, kS, kLast, data, 2, par);
  for (uint8_t i = 0; i + 1 < kN; ++i) {
    rx.feed(kFid, i, 0, data[i], static_cast<uint16_t>(kS));
  }
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // LAST missing: N unknown, no attempt.
  rx.reset();
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, 2, 0, data[2], static_cast<uint16_t>(kS));
  rx.feed(kFid, 3, uartpoc::FLAG_LAST_CHUNK, data[3], static_cast<uint16_t>(kLast));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(1, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>((kN - 1) * kS + kLast), rx.bytes);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data[1], slot + kS, kS);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data[3], slot + 3 * kS, kLast);
  TEST_ASSERT_EQUAL_UINT8(0, slot[3 * kS + kLast]);
  TEST_ASSERT_EQUAL_UINT8(0, slot[4 * kS - 1]);
}

void test_s3fec_corrupt_parity_drops() {
  // Corrupted parity: both rows buffered before LAST, so the solve sees a
  // redundant good row and the corrupt one trips UNRECOVERABLE — never
  // emitted; the existing drop path owns the frame.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 4;
  const size_t kS = 32;
  const uint16_t kFid = 46;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0x12);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  for (size_t b = 0; b < kS; ++b) {
    par[1][b] = static_cast<uint8_t>(par[1][b] ^ 0xFF);  // Needed row corrupted.
  }
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, 2, 0, data[2], static_cast<uint16_t>(kS));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // No LAST yet: buffered, no attempt.
  rx.feed(kFid, 3, uartpoc::FLAG_LAST_CHUNK, data[3], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // Redundant cross-check fails: UNRECOVERABLE.
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  rx.stale();
  TEST_ASSERT_EQUAL_UINT32(1, rx.drops);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
}

void test_s3fec_stale_flushes_parity() {
  // STALE flushes parity slots: the re-push starts clean, recovers nothing spuriously.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 4;
  const size_t kS = 32;
  const uint16_t kFid = 47;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0x34);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, 1, 0, data[1], static_cast<uint16_t>(kS));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT8(1, rx.pn);
  rx.stale();
  TEST_ASSERT_EQUAL_UINT32(1, rx.drops);
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);  // Parity flushed with the frame.
  for (uint8_t i = 0; i < kN; ++i) {
    const bool last = (i + 1 == kN);
    rx.feed(kFid, i, last ? uartpoc::FLAG_LAST_CHUNK : 0, data[i], static_cast<uint16_t>(kS));
  }
  TEST_ASSERT_EQUAL_UINT32(1, rx.ok);   // Fresh complete is clean...
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // ...never spuriously recovered.
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kN * kS), rx.bytes);
}

void test_s3fec_oversize_never_recovers() {
  // Cap overflow is not an erasure: OVERSIZE path, big counted, rec untouched.
  uint8_t slot[48];  // Smaller than one full frame.
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 3;
  const size_t kS = 32;
  const uint16_t kFid = 48;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0x56);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);                      // No LAST yet: no attempt.
  rx.feed(kFid, 1, 0, data[1], static_cast<uint16_t>(kS));  // off 32 + 32 > 48: OVERSIZE.
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(1, rx.drops);
  TEST_ASSERT_EQUAL_UINT32(1, rx.big);
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);
}

void test_s3fec_k0_parity_ignored() {
  // K=0: parity-bearing input is ignored safely; data path byte-identical.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  uint8_t junk[16] = {};
  rx.feed(20, 3, uartpoc::fec::kFlagParity, junk, sizeof(junk));
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(0, rx.drops);
  TEST_ASSERT_EQUAL_UINT32(0, rx.bytes);
  const uint8_t kN = 3;
  const size_t kS = 16;
  const uint16_t kFid = 20;
  uint8_t data[8][64];
  s3fec::fillData(kN, kS, kS, data, 0x78);
  for (uint8_t i = 0; i < kN; ++i) {
    const bool last = (i + 1 == kN);
    rx.feed(kFid, i, last ? uartpoc::FLAG_LAST_CHUNK : 0, data[i], static_cast<uint16_t>(kS));
  }
  TEST_ASSERT_EQUAL_UINT32(1, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(0, rx.drops);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kN * kS), rx.bytes);
}

void test_s3fec_chunks_data_only_parity_silent() {
  // S3 chunksRx_ counts DATA chunks only; parity never touches it (Head twin:
  // txChunks_ data-only with txParity_/txp separate). Pinned via the S3-side
  // mirror counter chunks — both suites share this S3-policy harness, so the
  // test is identical core<->head; there is no Head-side chunk counter to
  // mirror, hence no Head-specific variant.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 3;
  const size_t kS = 16;
  const uint16_t kFid = 51;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0xA1);
  s3fec::makeParity(kN, kS, kS, data, 1, par);
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, 1, 0, data[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(2, rx.chunks);
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));  // Duplicate: still silent.
  TEST_ASSERT_EQUAL_UINT32(2, rx.chunks);                                           // Parity never counts as chunks...
  TEST_ASSERT_EQUAL_UINT32(1, rx.prx);  // ...but the accepted store counts once.
  rx.feed(kFid, 2, uartpoc::FLAG_LAST_CHUNK, data[2], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(3, rx.chunks);
  TEST_ASSERT_EQUAL_UINT32(1, rx.ok);  // Clean complete (nothing was lost).
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(1, rx.prx);  // COMPLETE flush drops slots, never the cumulative counter.
}

void test_s3fec_parrx_counts_accepted_stores() {
  // S3 parRx_ (firmware poc_link.h onParity_) increments on every ACCEPTED
  // parity store; foreign-frame / duplicate / empty / oversize parity stays
  // silent with no counter. Mirror pins it via prx.
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const size_t kS = 16;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(3, kS, kS, data, 0xB2);
  s3fec::makeParity(3, kS, kS, data, 2, par);
  uint8_t junk[16] = {};
  rx.feed(60, 3, uartpoc::fec::kFlagParity, junk, sizeof(junk));  // No active frame: silent.
  TEST_ASSERT_EQUAL_UINT32(0, rx.prx);
  rx.feed(52, 0, 0, data[0], static_cast<uint16_t>(kS));                         // Activates frame 52.
  rx.feed(53, 3, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));  // Foreign frameId: silent.
  TEST_ASSERT_EQUAL_UINT32(0, rx.prx);
  rx.feed(52, 3, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(1, rx.prx);
  rx.feed(52, 3, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));  // Duplicate: silent.
  TEST_ASSERT_EQUAL_UINT32(1, rx.prx);
  rx.feed(52, 4, uartpoc::fec::kFlagParity, nullptr, 0);  // Empty: silent.
  TEST_ASSERT_EQUAL_UINT32(1, rx.prx);
  uint8_t big[65] = {};
  rx.feed(52, 5, uartpoc::fec::kFlagParity, big, sizeof(big));  // Oversize row: silent.
  TEST_ASSERT_EQUAL_UINT32(1, rx.prx);
  rx.feed(52, 4, uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(2, rx.prx);
}

void test_s3fec_stats_order_appended_last() {
  // STATS field order: S3 appends rec= then par= LAST (existing order
  // undisturbed); Head appends txp= then txdrop= then pumpMs= then grabMs=
  // LAST. See the formatS3Stats / formatHeadStats contract mirrors above for
  // the firmware sites.
  char s3[288];
  s3fec::formatS3Stats(s3, sizeof(s3), 1, 4, 0, 1, 0, 0, 0, 0, 0, 2, 128, 1, 0, 50000, 1000, 1, 2);
  TEST_ASSERT_EQUAL_STRING(
      "STATS ok=1 chunks=4 herr=0 perr=1 drops=0 big=0 ooo=0 dups=0 ovf=0 hb=2 bytes=128 kbps=1 kbs=0 intfree=50000 "
      "up=1000 rec=1 par=2",
      s3);
  const char* up = strstr(s3, " up=");
  const char* rec = strstr(s3, " rec=");
  const char* par = strstr(s3, " par=");
  TEST_ASSERT_NOT_NULL(up);
  TEST_ASSERT_NOT_NULL(rec);
  TEST_ASSERT_NOT_NULL(par);
  TEST_ASSERT_TRUE(up < rec && rec < par);  // rec/par trail every older field; par trails rec.
  TEST_ASSERT_NULL(strchr(par + 1, ' '));   // par= is the final token.
  char head[224];
  s3fec::formatHeadStats(head, sizeof(head), 7, 64, 4096, 3, 0, 1, 128, 1000, 115200, 10, 49000, 8, 1, 35, 2);
  TEST_ASSERT_EQUAL_STRING(
      "STATS txf=7 txc=64 txb=4096 rxcmd=3 rxe=0 mode=1 chunk=128 pace=1000 baud=115200 fps=10 intfree=49000 txp=8 "
      "txdrop=1 pumpMs=35 grabMs=2",
      head);
  const char* intfree = strstr(head, " intfree=");
  const char* txp = strstr(head, " txp=");
  const char* txdrop = strstr(head, " txdrop=");
  const char* pumpMs = strstr(head, " pumpMs=");
  const char* grabMs = strstr(head, " grabMs=");
  TEST_ASSERT_NOT_NULL(intfree);
  TEST_ASSERT_NOT_NULL(txp);
  TEST_ASSERT_NOT_NULL(txdrop);
  TEST_ASSERT_NOT_NULL(pumpMs);
  TEST_ASSERT_NOT_NULL(grabMs);
  TEST_ASSERT_TRUE(intfree < txp && txp < txdrop && txdrop < pumpMs &&
                   pumpMs < grabMs);          // txp/txdrop/pumpMs/grabMs trail every older field, in that order.
  TEST_ASSERT_NULL(strchr(grabMs + 1, ' '));  // grabMs= is the final token.
}

void test_s3fec_reset_clears_rec() {
  // RESET clears recFec_ (and parRx_) alongside every other counter; slots and
  // shadow reset with it, so no cross-frame leakage survives a RESET. Mirror
  // pins it via Rx::reset (firmware RESET additionally resets the decoder,
  // which has no mirror state here).
  uint8_t slot[512];
  uint8_t pbuf[4][64];
  s3fec::Rx rx;
  rx.attach(slot, sizeof(slot), &pbuf[0][0]);
  const uint8_t kN = 4;
  const size_t kS = 32;
  const uint16_t kFid = 54;
  uint8_t data[8][64];
  uint8_t par[4][64];
  s3fec::fillData(kN, kS, kS, data, 0xC3);
  s3fec::makeParity(kN, kS, kS, data, 2, par);
  rx.feed(kFid, 0, 0, data[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, 2, 0, data[2], static_cast<uint16_t>(kS));
  rx.feed(kFid, kN, uartpoc::fec::kFlagParity, par[0], static_cast<uint16_t>(kS));
  rx.feed(kFid, static_cast<uint16_t>(kN + 1), uartpoc::fec::kFlagParity, par[1], static_cast<uint16_t>(kS));
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);  // LAST unknown yet: buffered, no attempt.
  TEST_ASSERT_EQUAL_UINT32(2, rx.prx);
  rx.feed(kFid, 3, uartpoc::FLAG_LAST_CHUNK, data[3], static_cast<uint16_t>(kS));  // idx1 still missing: recovers.
  TEST_ASSERT_EQUAL_UINT32(1, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(2, rx.prx);
  TEST_ASSERT_EQUAL_UINT32(3, rx.chunks);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kN * kS), rx.bytes);
  rx.reset();  // Mirrors firmware RESET.
  TEST_ASSERT_EQUAL_UINT32(0, rx.rec);
  TEST_ASSERT_EQUAL_UINT32(0, rx.prx);
  TEST_ASSERT_EQUAL_UINT32(0, rx.ok);
  TEST_ASSERT_EQUAL_UINT32(0, rx.chunks);
  TEST_ASSERT_EQUAL_UINT32(0, rx.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, rx.drops);
  TEST_ASSERT_EQUAL_UINT32(0, rx.big);
  TEST_ASSERT_EQUAL_UINT8(0, rx.pn);
}

// ── Pending-fb overlap mirror (Head poc_manager.h slot state machine) ──
// Arduino-free model of the staging + pendingFb_ slots: fake fb ids stand in
// for camera_fb_t*. Fast path (stage-and-release): grabNext_ copies into
// INTERNAL staging and returns the fb immediately (staged_ set, no fb held);
// pumpStaged_ ships the copy. Legacy path (oversize): the driver queue (cap
// 2 = fb_count=2) feeds grabNext_; pump, hold-entry, and switch-entry mirror
// the firmware paths 1:1 (grab-two behind check, return-on-all-paths, txdrop
// ordering in STATS with pumpMs=/grabMs= trailing it). Any firmware lifetime
// change must update this mirror (both suites share it, so this block is
// identical core<->head; runners differ by suite-specific tests — S3-only
// bridge/ring mirrors live in core only).
namespace pendfb {

struct Slot {
  static const int kEmpty = -1;
  static const int kMaxIds = 8;
  int pending = kEmpty;  // Fake fb id held across ticks, legacy path (kEmpty = none).
  int staged = kEmpty;   // Fake fb id staged in INTERNAL copy, fast path (kEmpty = none).
  uint32_t frames = 0;   // Completed pumps (txFrames_ mirror).
  uint32_t drops = 0;    // Stale drops (txDropStale_ mirror).
  int ret[kMaxIds];      // Return-count per fake id (double-return detector, legacy path).

  Slot() {
    for (int i = 0; i < kMaxIds; ++i) {
      ret[i] = 0;
    }
  }
};

inline void release(Slot& s, int id) { ++s.ret[id]; }  // esp_camera_fb_return stand-in.

// Mirrors grabNext_ fast path: stage the copy (+1 implicit immediate return,
// so no ret[] entry); a still-staged previous frame means behind -> drop the
// older staged frame, count it, stage the newer.
inline void stageGrab(Slot& s, int id) {
  if (s.staged != Slot::kEmpty) {
    s.staged = Slot::kEmpty;
    ++s.drops;
  }
  s.staged = id;
}

// Mirrors pumpStaged_ completion: staged copy shipped -> clear, count frame.
// No fb_return by construction (the live fb left at stage time).
inline void pumpStaged(Slot& s) {
  if (s.staged < 0) {
    return;
  }
  s.staged = Slot::kEmpty;
  ++s.frames;
}

// Mirrors PocManager::grabNext_: adopt the oldest queued frame; a second
// queued frame means behind -> return the stale older one, count it.
inline void grabNext(Slot& s, int first, int second) {  // -1 = none queued.
  if (first < 0) {
    return;
  }
  TEST_ASSERT_EQUAL_INT(Slot::kEmpty, s.pending);  // Pump-then-grab: slot empty here.
  if (second >= 0) {
    release(s, first);
    ++s.drops;
    first = second;
  }
  s.pending = first;
}

// Mirrors pumpPending_ completion: last chunk staged -> return, count frame.
inline void pump(Slot& s) {
  if (s.pending < 0) {
    return;
  }
  const int id = s.pending;
  s.pending = Slot::kEmpty;
  release(s, id);
  ++s.frames;
}

// Mirrors the tick-head hold drop and switchFramesize_ return-and-drop:
// return pending, counted nowhere (never shipped); staging cleared alongside
// (dropPending_ clears stagedValid_ — a stale copy must never ship after a
// hold/switch window).
inline void holdEntry(Slot& s) {
  if (s.pending >= 0) {
    const int id = s.pending;
    s.pending = Slot::kEmpty;
    release(s, id);
  }
  s.staged = Slot::kEmpty;
}

inline void switchEntry(Slot& s) { holdEntry(s); }

}  // namespace pendfb

void test_pendingfb_send_completes_returns() {
  // Steady state: grab one (driver kept up) -> pump completes -> the fb is
  // returned exactly once, the frame counted, nothing dropped.
  pendfb::Slot s;
  pendfb::grabNext(s, 3, -1);
  TEST_ASSERT_EQUAL_INT(3, s.pending);
  pendfb::pump(s);
  TEST_ASSERT_EQUAL_INT(pendfb::Slot::kEmpty, s.pending);
  TEST_ASSERT_EQUAL_UINT32(1, s.frames);
  TEST_ASSERT_EQUAL_UINT32(0, s.drops);
  TEST_ASSERT_EQUAL_INT(1, s.ret[3]);
}

void test_pendingfb_stale_dropped_counted() {
  // Behind: while frame 1 pumped, the driver queued 2 AND 3 -> the stale
  // older frame (2) is returned + counted, the newer (3) ships.
  pendfb::Slot s;
  pendfb::grabNext(s, 1, -1);
  pendfb::pump(s);  // Tick 1 completes: frames=1, fb 1 returned.
  TEST_ASSERT_EQUAL_UINT32(1, s.frames);
  pendfb::grabNext(s, 2, 3);  // Tick 2 grab: behind.
  TEST_ASSERT_EQUAL_UINT32(1, s.drops);
  TEST_ASSERT_EQUAL_INT(1, s.ret[2]);  // Stale returned...
  TEST_ASSERT_EQUAL_INT(0, s.ret[3]);  // ...newer not yet.
  TEST_ASSERT_EQUAL_INT(3, s.pending);
  pendfb::pump(s);
  TEST_ASSERT_EQUAL_UINT32(2, s.frames);
  TEST_ASSERT_EQUAL_INT(1, s.ret[1]);
  TEST_ASSERT_EQUAL_INT(1, s.ret[2]);
  TEST_ASSERT_EQUAL_INT(1, s.ret[3]);
  TEST_ASSERT_EQUAL_INT(pendfb::Slot::kEmpty, s.pending);
}

void test_pendingfb_switch_entry_returns_pending() {
  // A fb held across the tick boundary (grabbed, not yet pumped) is returned
  // by the switch entry before any deinit — uncounted either way.
  pendfb::Slot s;
  pendfb::grabNext(s, 4, -1);
  pendfb::switchEntry(s);
  TEST_ASSERT_EQUAL_INT(pendfb::Slot::kEmpty, s.pending);
  TEST_ASSERT_EQUAL_INT(1, s.ret[4]);
  TEST_ASSERT_EQUAL_UINT32(0, s.frames);
  TEST_ASSERT_EQUAL_UINT32(0, s.drops);
}

void test_pendingfb_no_double_return() {
  // Mixed journey over ids 0..5 (completes + stale + hold + switch drops):
  // every touched fb is returned EXACTLY once, the slot ends empty.
  pendfb::Slot s;
  pendfb::grabNext(s, 0, -1);
  pendfb::pump(s);            // Complete: frames=1.
  pendfb::grabNext(s, 1, 2);  // Behind: fb 1 stale (drops=1), fb 2 pending.
  pendfb::holdEntry(s);       // Hold entry: fb 2 returned, uncounted.
  pendfb::grabNext(s, 3, 4);  // Behind again: fb 3 stale (drops=2), fb 4 pending.
  pendfb::switchEntry(s);     // Switch entry: fb 4 returned, uncounted.
  pendfb::grabNext(s, 5, -1);
  pendfb::pump(s);  // Complete: frames=2.
  TEST_ASSERT_EQUAL_UINT32(2, s.frames);
  TEST_ASSERT_EQUAL_UINT32(2, s.drops);
  TEST_ASSERT_EQUAL_INT(pendfb::Slot::kEmpty, s.pending);
  for (int i = 0; i <= 5; ++i) {
    TEST_ASSERT_EQUAL_INT(1, s.ret[i]);
  }
  for (int i = 6; i < pendfb::Slot::kMaxIds; ++i) {
    TEST_ASSERT_EQUAL_INT(0, s.ret[i]);
  }
}

// ── Firmware identity markers (main.cpp BOOT banners) ──
// Manual fw= literals bumped on every firmware change (hash verifies
// bytes-written, not source-identity — the banner proves which image runs).
// Must stay byte-identical to head/src/main.cpp + core/src/main.cpp banners;
// any firmware change bumps the markers here too. (Identical core<->head.)
namespace fwmarker {

inline const char* headFw() { return "fw=head-006-decouple"; }
inline const char* s3Fw() { return "fw=s3-006-ring16+split"; }

}  // namespace fwmarker

void test_pendingfb_staged_collision_counts_drop() {
  // Fast path steady state: stage A -> pump ships it (frames=1, no fb held,
  // no return entries — the live fb left at stage time) -> stage B -> pump.
  pendfb::Slot s;
  pendfb::stageGrab(s, 0);
  TEST_ASSERT_EQUAL_INT(0, s.ret[0]);  // Immediate return is implicit: no ret[] entry, nothing held.
  pendfb::pumpStaged(s);
  TEST_ASSERT_EQUAL_UINT32(1, s.frames);
  TEST_ASSERT_EQUAL_UINT32(0, s.drops);
  // Collision: stage C, then stage D without pumping (behind) -> older
  // staged C dropped + counted, D ships on the next pump.
  pendfb::stageGrab(s, 2);
  pendfb::stageGrab(s, 3);
  TEST_ASSERT_EQUAL_UINT32(1, s.drops);
  pendfb::pumpStaged(s);
  TEST_ASSERT_EQUAL_UINT32(2, s.frames);
  TEST_ASSERT_EQUAL_INT(pendfb::Slot::kEmpty, s.staged);
  // Hold entry with staging set: cleared, uncounted, never shipped.
  pendfb::stageGrab(s, 4);
  pendfb::holdEntry(s);
  TEST_ASSERT_EQUAL_INT(pendfb::Slot::kEmpty, s.staged);
  TEST_ASSERT_EQUAL_UINT32(2, s.frames);
  TEST_ASSERT_EQUAL_UINT32(1, s.drops);
}

void test_fw_markers_pinned() {
  // Current firmware revs (bump with every firmware change, both suites):
  // the banner marker is the image-identity proof on console.
  TEST_ASSERT_EQUAL_STRING("fw=head-006-decouple", fwmarker::headFw());
  TEST_ASSERT_EQUAL_STRING("fw=s3-006-ring16+split", fwmarker::s3Fw());
}
