// Host unit tests for the UART POC codec + config (no hardware).
// Run: pio test -e native-test
//
// NOTE endianness: uart_frame.h is little-endian by design (ESP<->ESP only),
// a deliberate deviation from middleware/udp_codec.h (big-endian app link).

#include <unity.h>

#include "link/poc_config.h"
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
  pushChunk(r, 9, 1, true, c1, sizeof(c1), uartpoc::Reassembler::Push::DROPPED);
  TEST_ASSERT_FALSE(r.active());
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
  TEST_ASSERT_TRUE(uartpoc::parseSet("baud", "230400", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_UINT32(230400, cfg.baud);
  TEST_ASSERT_EQUAL_STRING("ACK baud 230400", msg);
}

void test_uart_config_reject() {
  uartpoc::PocConfig cfg;
  char msg[64];
  TEST_ASSERT_FALSE(uartpoc::parseSet("baud", "12345", cfg, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("NACK bad_baud", msg);
  TEST_ASSERT_EQUAL_UINT32(115200, cfg.baud);  // Rejected SET leaves config untouched.
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
  cfg.baud = 230400;
  cfg.mode = 2;
  cfg.fps = 5;
  char out[128];
  uartpoc::formatGet(cfg, "chunk", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("chunk 64", out);
  uartpoc::formatGet(cfg, "baud", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("baud 230400", out);
  uartpoc::formatGet(cfg, "all", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("chunk 64 pace 500 baud 230400 mode 2 fps 5", out);
  uartpoc::formatGet(cfg, "nope", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("NACK unknown_key", out);
}
