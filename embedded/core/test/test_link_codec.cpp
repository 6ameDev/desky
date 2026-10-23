// Host unit tests for the core<->head link frame codec (B-link, no hardware).
// Run: pio test -e native-test
//
// Golden vectors pin the exact wire bytes (hand-computed, CRC-CCITT-FALSE
// cross-checked with an independent implementation):
//   FACE seq=1 frag=0/1 payload=[0x02]:
//     4C 01 00 01 00 01 00 01 02 95 0F
//   HB seq=0x0102 frag=0/1 payload=[BD 0B 00 03 0D 40 12 34 56 78]
//   (rssi=-67, ch=11, heap=200000, bootId=0x12345678):
//     4C 20 01 02 00 01 00 0A BD 0B 00 03 0D 40 12 34 56 78 D7 F7

#include <unity.h>

#include "link/link_codec.h"

void test_link_crc16_standard_vector() {
  // WS4: CRC16-CCITT-FALSE standard check — ASCII "123456789" -> 0x29B1
  // (the canonical published vector for poly 0x1021 / init 0xFFFF).
  const uint8_t msg[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_UINT16(0x29B1, link::crc16Ccitt(msg, sizeof(msg)));
}

void test_link_face_golden_encode() {
  const uint8_t payload[1] = {0x02};
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypeFace, 1, payload, sizeof(payload), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(11, n);
  const uint8_t expected[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
}

void test_link_face_golden_decode() {
  const uint8_t golden[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(golden, sizeof(golden), f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypeFace, f.type);
  TEST_ASSERT_EQUAL_UINT16(1, f.seq);
  TEST_ASSERT_EQUAL_UINT8(0, f.fragIdx);
  TEST_ASSERT_EQUAL_UINT8(1, f.fragTotal);
  TEST_ASSERT_EQUAL_UINT16(1, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8(0x02, f.payload[0]);
}

void test_link_hb_golden_encode() {
  uint8_t payload[link::kHbLen] = {};
  TEST_ASSERT_TRUE(link::packHb(-67, 11, 200000, 0x12345678u, payload));
  const uint8_t expectedPayload[10] = {0xBD, 0x0B, 0x00, 0x03, 0x0D, 0x40, 0x12, 0x34, 0x56, 0x78};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedPayload, payload, sizeof(payload));
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypeHb, 0x0102, payload, sizeof(payload), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(20, n);
  const uint8_t expected[20] = {0x4C, 0x20, 0x01, 0x02, 0x00, 0x01, 0x00, 0x0A, 0xBD, 0x0B,
                                0x00, 0x03, 0x0D, 0x40, 0x12, 0x34, 0x56, 0x78, 0xD7, 0xF7};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
}

void test_link_hb_golden_decode() {
  const uint8_t golden[20] = {0x4C, 0x20, 0x01, 0x02, 0x00, 0x01, 0x00, 0x0A, 0xBD, 0x0B,
                              0x00, 0x03, 0x0D, 0x40, 0x12, 0x34, 0x56, 0x78, 0xD7, 0xF7};
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(golden, sizeof(golden), f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypeHb, f.type);
  TEST_ASSERT_EQUAL_UINT16(0x0102, f.seq);
  TEST_ASSERT_EQUAL_UINT8(0, f.fragIdx);
  TEST_ASSERT_EQUAL_UINT8(1, f.fragTotal);
  TEST_ASSERT_EQUAL_UINT16(link::kHbLen, f.payloadLen);
  int8_t rssi = 0;
  uint8_t channel = 0;
  uint32_t freeHeap = 0;
  uint32_t bootId = 0;
  TEST_ASSERT_TRUE(link::unpackHb(f.payload, f.payloadLen, rssi, channel, freeHeap, bootId));
  TEST_ASSERT_EQUAL_INT8(-67, rssi);
  TEST_ASSERT_EQUAL_UINT8(11, channel);
  TEST_ASSERT_EQUAL_UINT32(200000, freeHeap);
  TEST_ASSERT_EQUAL_UINT32(0x12345678u, bootId);
  // Legacy 6B HB (pre-bootId) is rejected: wrong length for the contract.
  TEST_ASSERT_FALSE(link::unpackHb(f.payload, 6, rssi, channel, freeHeap, bootId));
}

void test_link_rejects_bad_magic() {
  link::Frame f;
  // Phone-wire 0xAA header as the sync byte: never accepted on the link wire.
  const uint8_t phoneStyle[11] = {0xAA, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
  TEST_ASSERT_FALSE(link::decodeFrame(phoneStyle, sizeof(phoneStyle), f));
  const uint8_t badMagic[11] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
  TEST_ASSERT_FALSE(link::decodeFrame(badMagic, sizeof(badMagic), f));
  // Truncated below the header+CRC floor.
  const uint8_t shortBuf[9] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02};
  TEST_ASSERT_FALSE(link::decodeFrame(shortBuf, sizeof(shortBuf), f));
}

void test_link_rejects_bad_crc() {
  link::Frame f;
  const uint8_t golden[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F};
  uint8_t flipped[sizeof(golden)];
  for (size_t i = 0; i < sizeof(golden); ++i) {
    flipped[i] = golden[i];
  }
  flipped[8] ^= 0x01;  // Payload bit flip: CRC must fail.
  TEST_ASSERT_FALSE(link::decodeFrame(flipped, sizeof(flipped), f));
  for (size_t i = 0; i < sizeof(golden); ++i) {
    flipped[i] = golden[i];
  }
  flipped[10] ^= 0xFF;  // CRC byte flip: must fail.
  TEST_ASSERT_FALSE(link::decodeFrame(flipped, sizeof(flipped), f));
}

void test_link_rejects_length_mismatch() {
  link::Frame f;
  // Len field claims 2 payload bytes, but only 1 rides the buffer.
  const uint8_t lieLen[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x95, 0x0F};
  TEST_ASSERT_FALSE(link::decodeFrame(lieLen, sizeof(lieLen), f));
  // Trailing garbage past the framed length.
  const uint8_t garbage[12] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x95, 0x0F, 0x00};
  TEST_ASSERT_FALSE(link::decodeFrame(garbage, sizeof(garbage), f));
}

void test_link_null_safety() {
  const uint8_t payload[1] = {0x02};
  uint8_t out[16] = {};
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 1, 0, 1, nullptr, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, nullptr, 1, 0, 1, out, sizeof(out)));
  link::Frame f;
  TEST_ASSERT_FALSE(link::decodeFrame(nullptr, 11, f));
  TEST_ASSERT_FALSE(link::decodeFrame(out, 0, f));
  TEST_ASSERT_EQUAL_UINT16(0, link::crc16Ccitt(nullptr, 5));
  TEST_ASSERT_FALSE(link::packHb(0, 0, 0, 0, nullptr));
  int8_t rssi = 0;
  uint8_t channel = 0;
  uint32_t heap = 0;
  uint32_t boot = 0;
  TEST_ASSERT_FALSE(link::unpackHb(nullptr, link::kHbLen, rssi, channel, heap, boot));
  link::Reassembler r;
  r.reset(nullptr, 8);
  TEST_ASSERT_FALSE(r.push(0, 1, payload, 1));
  r.reset(out, sizeof(out));
  TEST_ASSERT_FALSE(r.push(0, 1, nullptr, 1));
}

void test_link_encode_rejects_bad_args() {
  const uint8_t payload[1] = {0x02};
  uint8_t out[link::kMaxFrame] = {};
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 1, 0, 0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 1, 1, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 1, 2, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(0x09, 1, payload, 1, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(0xFF, 1, payload, 1, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 1, 0, 1, out, 10));
  uint8_t big[1025];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = static_cast<uint8_t>(i & 0xFFu);
  }
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeJpeg, 1, big, sizeof(big), 0, 1, out, sizeof(out)));
}

void test_link_decode_rejects_bad_frag_and_unknown_type() {
  link::Frame f;
  // fragTotal == 0 with a matching CRC is still rejected.
  uint8_t zeroTotal[11] = {0x4C, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00};
  link::putU16BE(zeroTotal + 9, link::crc16Ccitt(zeroTotal, 9));
  TEST_ASSERT_FALSE(link::decodeFrame(zeroTotal, sizeof(zeroTotal), f));
  // fragIdx >= fragTotal with a matching CRC is still rejected.
  uint8_t idxHigh[11] = {0x4C, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x02, 0x00, 0x00};
  link::putU16BE(idxHigh + 9, link::crc16Ccitt(idxHigh, 9));
  TEST_ASSERT_FALSE(link::decodeFrame(idxHigh, sizeof(idxHigh), f));
  // Unknown type with a matching CRC is still rejected (frozen contract).
  uint8_t unknown[11] = {0x4C, 0x09, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x00};
  link::putU16BE(unknown + 9, link::crc16Ccitt(unknown, 9));
  TEST_ASSERT_FALSE(link::decodeFrame(unknown, sizeof(unknown), f));
}

void test_link_jpeg_max_boundary() {
  uint8_t big[link::kMaxPayload];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = static_cast<uint8_t>(i & 0xFFu);
  }
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypeJpeg, 0xBEEF, big, sizeof(big), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(link::kMaxFrame, n);
  TEST_ASSERT_EQUAL_size_t(1034, n);
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(out, n, f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypeJpeg, f.type);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, f.seq);
  TEST_ASSERT_EQUAL_UINT16(link::kMaxPayload, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(big, f.payload, sizeof(big));
  // Empty JPEG payload still rides one frame.
  const size_t empty = link::encodeFrame(link::kTypeJpeg, 2, big, 0, 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(link::kOverhead, empty);
}

void test_link_fixed_type_lengths() {
  uint8_t payload[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  uint8_t out[link::kMaxFrame] = {};
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 0, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeFace, 1, payload, 2, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeOled, 1, payload, 0, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeCamCtl, 1, payload, 2, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeHb, 1, payload, 5, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeHbAck, 1, payload, 4, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeAck, 1, payload, 1, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeAck, 1, payload, 3, 0, 1, out, sizeof(out)));
  // Valid singletons encode.
  TEST_ASSERT_EQUAL_size_t(11, link::encodeFrame(link::kTypeOled, 1, payload, 1, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(11, link::encodeFrame(link::kTypeCamCtl, 1, payload, 1, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(15, link::encodeFrame(link::kTypeHbAck, 1, payload, 5, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(12, link::encodeFrame(link::kTypeAck, 1, payload, 2, 0, 1, out, sizeof(out)));
}

void test_link_round_trip_all_types() {
  struct Case {
    uint8_t type;
    uint16_t len;
  };
  static const Case cases[] = {
      {link::kTypeFace, 1}, {link::kTypeOled, 1},  {link::kTypeCamCtl, 1}, {link::kTypeJpeg, 7},
      {link::kTypeHb, 10},  {link::kTypeHbAck, 5}, {link::kTypeAck, 2},
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    uint8_t payload[63];
    for (size_t i = 0; i < cases[c].len; ++i) {
      payload[i] = static_cast<uint8_t>((c * 17u + i) & 0xFFu);
    }
    uint8_t out[link::kMaxFrame] = {};
    const uint16_t seq = static_cast<uint16_t>(100 + c);
    const size_t n = link::encodeFrame(cases[c].type, seq, payload, cases[c].len, 0, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(link::kOverhead + cases[c].len, n);
    link::Frame f;
    TEST_ASSERT_TRUE(link::decodeFrame(out, n, f));
    TEST_ASSERT_EQUAL_UINT8(cases[c].type, f.type);
    TEST_ASSERT_EQUAL_UINT16(seq, f.seq);
    TEST_ASSERT_EQUAL_UINT16(cases[c].len, f.payloadLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, f.payload, cases[c].len);
  }
}

void test_link_hb_ack_ack_pack() {
  uint8_t hbAck[link::kHbAckLen] = {};
  TEST_ASSERT_TRUE(link::packHbAck(11, 0x12345678, hbAck));
  const uint8_t expectedHbAck[5] = {0x0B, 0x12, 0x34, 0x56, 0x78};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedHbAck, hbAck, sizeof(hbAck));
  uint8_t channel = 0;
  uint32_t era = 0;
  TEST_ASSERT_TRUE(link::unpackHbAck(hbAck, sizeof(hbAck), channel, era));
  TEST_ASSERT_EQUAL_UINT8(11, channel);
  TEST_ASSERT_EQUAL_UINT32(0x12345678, era);
  TEST_ASSERT_FALSE(link::packHbAck(0, 0, nullptr));
  TEST_ASSERT_FALSE(link::unpackHbAck(hbAck, 4, channel, era));
  uint8_t ack[link::kAckLen] = {};
  TEST_ASSERT_TRUE(link::packAck(0xABCD, ack));
  const uint8_t expectedAck[2] = {0xAB, 0xCD};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedAck, ack, sizeof(ack));
  uint16_t acked = 0;
  TEST_ASSERT_TRUE(link::unpackAck(ack, sizeof(ack), acked));
  TEST_ASSERT_EQUAL_UINT16(0xABCD, acked);
  TEST_ASSERT_FALSE(link::packAck(0, nullptr));
  TEST_ASSERT_FALSE(link::unpackAck(ack, 1, acked));
}

void test_link_fresh_seq_golden() {
  TEST_ASSERT_TRUE(link::isFreshSeq(41, 42));
  TEST_ASSERT_TRUE(link::isFreshSeq(0, 1));
  TEST_ASSERT_TRUE(link::isFreshSeq(0, 0x7FFF));
  TEST_ASSERT_FALSE(link::isFreshSeq(42, 42));
  TEST_ASSERT_FALSE(link::isFreshSeq(42, 41));
  TEST_ASSERT_FALSE(link::isFreshSeq(1, 0));
  TEST_ASSERT_FALSE(link::isFreshSeq(0, 0xFFFF));
}

void test_link_fresh_seq_wrap() {
  TEST_ASSERT_TRUE(link::isFreshSeq(0xFFFF, 0x0000));
  TEST_ASSERT_TRUE(link::isFreshSeq(0xFFFE, 0x0002));
  TEST_ASSERT_TRUE(link::isFreshSeq(0x8000, 0x8001));
  TEST_ASSERT_FALSE(link::isFreshSeq(0x0000, 0xFFFF));
  TEST_ASSERT_FALSE(link::isFreshSeq(0x0002, 0xFFFE));
  TEST_ASSERT_FALSE(link::isFreshSeq(0, 0x8000));
  TEST_ASSERT_FALSE(link::isFreshSeq(100, static_cast<uint16_t>(100 + 0x8000)));
}

void test_link_mtu_helpers() {
  TEST_ASSERT_EQUAL_size_t(0, link::maxPayloadFor(9));
  TEST_ASSERT_EQUAL_size_t(0, link::maxPayloadFor(10));
  TEST_ASSERT_EQUAL_size_t(1, link::maxPayloadFor(11));
  TEST_ASSERT_EQUAL_size_t(54, link::maxPayloadFor(64));
  TEST_ASSERT_EQUAL_size_t(1024, link::maxPayloadFor(1034));
  TEST_ASSERT_EQUAL_size_t(1024, link::maxPayloadFor(100000));
  TEST_ASSERT_FALSE(link::needsFrag(54, 64));
  TEST_ASSERT_TRUE(link::needsFrag(55, 64));
  TEST_ASSERT_FALSE(link::needsFrag(1024, 1034));
  TEST_ASSERT_TRUE(link::needsFrag(1024, 64));
  TEST_ASSERT_EQUAL_size_t(19, link::fragCount(1024, 64));
  TEST_ASSERT_EQUAL_size_t(2, link::fragCount(108, 64));
  TEST_ASSERT_EQUAL_size_t(1, link::fragCount(54, 64));
  TEST_ASSERT_EQUAL_size_t(1, link::fragCount(0, 64));
  TEST_ASSERT_EQUAL_size_t(0, link::fragCount(100, 10));
}

void test_link_reassembly_in_order() {
  uint8_t buf[8] = {};
  link::Reassembler r;
  r.reset(buf, sizeof(buf));
  const uint8_t p0[2] = {0xAA, 0xBB};
  const uint8_t p1[2] = {0xCC, 0xDD};
  const uint8_t p2[1] = {0xEE};
  TEST_ASSERT_TRUE(r.push(0, 3, p0, sizeof(p0)));
  TEST_ASSERT_FALSE(r.complete());
  TEST_ASSERT_TRUE(r.push(1, 3, p1, sizeof(p1)));
  TEST_ASSERT_TRUE(r.push(2, 3, p2, sizeof(p2)));
  TEST_ASSERT_TRUE(r.complete());
  TEST_ASSERT_EQUAL_size_t(5, r.len());
  const uint8_t expected[5] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
  // Single-frame message completes after one push.
  r.reset(buf, sizeof(buf));
  TEST_ASSERT_TRUE(r.push(0, 1, p0, sizeof(p0)));
  TEST_ASSERT_TRUE(r.complete());
}

void test_link_reassembly_duplicate_reject() {
  uint8_t buf[8] = {};
  link::Reassembler r;
  r.reset(buf, sizeof(buf));
  const uint8_t p0[2] = {0x11, 0x22};
  const uint8_t p1[2] = {0x33, 0x44};
  TEST_ASSERT_TRUE(r.push(0, 2, p0, sizeof(p0)));
  TEST_ASSERT_FALSE(r.push(0, 2, p0, sizeof(p0)));  // Duplicate idx 0.
  TEST_ASSERT_EQUAL_size_t(2, r.len());
  TEST_ASSERT_TRUE(r.push(1, 2, p1, sizeof(p1)));
  TEST_ASSERT_TRUE(r.complete());
  TEST_ASSERT_FALSE(r.push(1, 2, p1, sizeof(p1)));  // Replayed tail.
  const uint8_t expected[4] = {0x11, 0x22, 0x33, 0x44};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, 4);
}

void test_link_reassembly_total_mismatch_reject() {
  uint8_t buf[8] = {};
  link::Reassembler r;
  r.reset(buf, sizeof(buf));
  const uint8_t p[2] = {0x01, 0x02};
  TEST_ASSERT_TRUE(r.push(0, 2, p, sizeof(p)));
  TEST_ASSERT_FALSE(r.push(1, 3, p, sizeof(p)));  // Total changed mid-message.
  TEST_ASSERT_FALSE(r.complete());
  TEST_ASSERT_EQUAL_size_t(2, r.len());
  TEST_ASSERT_TRUE(r.push(1, 2, p, sizeof(p)));  // Original total resumes.
  TEST_ASSERT_TRUE(r.complete());
}

void test_link_reassembly_overlap_and_gap_reject() {
  uint8_t buf[8] = {};
  link::Reassembler r;
  r.reset(buf, sizeof(buf));
  const uint8_t p[2] = {0x01, 0x02};
  TEST_ASSERT_FALSE(r.push(1, 2, p, sizeof(p)));  // Gap: idx 1 before idx 0.
  TEST_ASSERT_TRUE(r.push(0, 2, p, sizeof(p)));
  TEST_ASSERT_FALSE(r.push(0, 2, p, sizeof(p)));  // Overlap: idx 0 already written.
  TEST_ASSERT_FALSE(r.push(2, 3, p, sizeof(p)));  // Out-of-window idx with wrong total.
  TEST_ASSERT_FALSE(r.complete());
  TEST_ASSERT_EQUAL_size_t(2, r.len());
  TEST_ASSERT_TRUE(r.push(1, 2, p, sizeof(p)));
  TEST_ASSERT_TRUE(r.complete());
}

void test_link_reassembly_overflow_reject() {
  uint8_t buf[3] = {};
  link::Reassembler r;
  r.reset(buf, sizeof(buf));
  const uint8_t p2[2] = {0x01, 0x02};
  const uint8_t p1[1] = {0x03};
  TEST_ASSERT_TRUE(r.push(0, 2, p2, sizeof(p2)));
  TEST_ASSERT_FALSE(r.push(1, 2, p2, sizeof(p2)));  // 2 + 2 > cap 3.
  TEST_ASSERT_FALSE(r.complete());
  TEST_ASSERT_EQUAL_size_t(2, r.len());  // Failed push mutated nothing.
  TEST_ASSERT_TRUE(r.push(1, 2, p1, sizeof(p1)));
  TEST_ASSERT_TRUE(r.complete());
  const uint8_t expected[3] = {0x01, 0x02, 0x03};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

void test_link_pairshow_golden_pack() {
  // SHOW payload golden: digits "123456" + validSecs 300 (0x012C) packs to
  // the literal 8B below (computed once, pinned here).
  const uint8_t digits[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t out[link::kPairShowLen] = {};
  TEST_ASSERT_TRUE(link::packPairShow(digits, 300, out));
  const uint8_t expected[8] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x01, 0x2C};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
  // Unpack round-trips the same bytes.
  uint8_t back[6] = {};
  uint16_t secs = 0;
  TEST_ASSERT_TRUE(link::unpackPairShow(out, sizeof(out), back, secs));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, back, sizeof(digits));
  TEST_ASSERT_EQUAL_UINT16(300, secs);
}

void test_link_pairshow_golden_frame() {
  // SHOW frame golden: seq=9 frag=0/1 payload=[31 32 33 34 35 36 01 2C]:
  //   4C 06 00 09 00 01 00 08 31 32 33 34 35 36 01 2C 73 02
  // (CRC-CCITT-FALSE cross-checked with an independent implementation).
  const uint8_t golden[18] = {0x4C, 0x06, 0x00, 0x09, 0x00, 0x01, 0x00, 0x08, 0x31,
                              0x32, 0x33, 0x34, 0x35, 0x36, 0x01, 0x2C, 0x73, 0x02};
  const uint8_t digits[6] = {'1', '2', '3', '4', '5', '6'};
  uint8_t payload[link::kPairShowLen] = {};
  TEST_ASSERT_TRUE(link::packPairShow(digits, 300, payload));
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypePairShow, 9, payload, sizeof(payload), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(golden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, out, sizeof(golden));
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(golden, sizeof(golden), f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypePairShow, f.type);
  TEST_ASSERT_EQUAL_UINT16(9, f.seq);
  TEST_ASSERT_EQUAL_UINT16(link::kPairShowLen, f.payloadLen);
  uint8_t back[6] = {};
  uint16_t secs = 0;
  TEST_ASSERT_TRUE(link::unpackPairShow(f.payload, f.payloadLen, back, secs));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, back, sizeof(digits));
  TEST_ASSERT_EQUAL_UINT16(300, secs);
}

void test_link_pairshow_round_trip_edges() {
  // validSecs edges (0 = already-expired, 65535 = max) + boundary codes.
  static const uint8_t kCodes[3][6] = {
      {'0', '0', '0', '0', '0', '0'}, {'9', '9', '9', '9', '9', '9'}, {'1', '2', '3', '4', '5', '6'}};
  static const uint16_t kSecs[3] = {0, 65535, 1};
  for (int c = 0; c < 3; ++c) {
    uint8_t packed[link::kPairShowLen] = {};
    TEST_ASSERT_TRUE(link::packPairShow(kCodes[c], kSecs[c], packed));
    uint8_t digits[6] = {};
    uint16_t secs = 0xFFFF ^ kSecs[c];
    TEST_ASSERT_TRUE(link::unpackPairShow(packed, sizeof(packed), digits, secs));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kCodes[c], digits, 6);
    TEST_ASSERT_EQUAL_UINT16(kSecs[c], secs);
    // Frame round trip through the length gate.
    uint8_t out[link::kMaxFrame] = {};
    const size_t n = link::encodeFrame(link::kTypePairShow, static_cast<uint16_t>(c), packed, sizeof(packed), 0, 1, out,
                                       sizeof(out));
    TEST_ASSERT_EQUAL_size_t(link::kOverhead + link::kPairShowLen, n);
    link::Frame f;
    TEST_ASSERT_TRUE(link::decodeFrame(out, n, f));
    TEST_ASSERT_EQUAL_UINT8(link::kTypePairShow, f.type);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(packed, f.payload, sizeof(packed));
  }
}

void test_link_pairshow_rejects() {
  const uint8_t digits[6] = {'1', '2', '3', '4', '5', '6'};
  const uint8_t nonDigit[6] = {'1', '2', '3', '4', '5', 'X'};
  uint8_t out[link::kPairShowLen] = {};
  uint8_t back[6] = {};
  uint16_t secs = 0;
  // Fail-closed on non-digits both ways (mirrors qr::encode6Digits).
  TEST_ASSERT_FALSE(link::packPairShow(nonDigit, 300, out));
  TEST_ASSERT_FALSE(link::packPairShow(digits, 300, nullptr));
  TEST_ASSERT_FALSE(link::packPairShow(nullptr, 300, out));
  uint8_t bad[link::kPairShowLen] = {'1', '2', '3', '4', '5', 'X', 0x01, 0x2C};
  TEST_ASSERT_FALSE(link::unpackPairShow(bad, sizeof(bad), back, secs));
  // Length gate: 7B and 9B never encode; hand-built frames with valid CRCs
  // still fail decode.
  uint8_t frame[link::kMaxFrame] = {};
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypePairShow, 1, out, 7, 0, 1, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypePairShow, 1, out, 9, 0, 1, frame, sizeof(frame)));
  TEST_ASSERT_FALSE(link::unpackPairShow(out, 7, back, secs));
  TEST_ASSERT_FALSE(link::unpackPairShow(out, 9, back, secs));
  TEST_ASSERT_FALSE(link::unpackPairShow(nullptr, link::kPairShowLen, back, secs));
  TEST_ASSERT_FALSE(link::unpackPairShow(out, link::kPairShowLen, nullptr, secs));
  uint8_t short7[link::kOverhead + 7] = {0x4C, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x07,
                                         0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x01};
  link::putU16BE(short7 + link::kHeaderSize + 7, link::crc16Ccitt(short7, link::kHeaderSize + 7));
  link::Frame f;
  TEST_ASSERT_FALSE(link::decodeFrame(short7, sizeof(short7), f));
}

void test_link_pairdone_empty_frame() {
  // DONE golden: empty payload, seq=10 frag=0/1:
  //   4C 07 00 0A 00 01 00 00 BE 83
  const uint8_t golden[10] = {0x4C, 0x07, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x00, 0xBE, 0x83};
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypePairDone, 10, nullptr, 0, 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(golden), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, out, sizeof(golden));
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(golden, sizeof(golden), f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypePairDone, f.type);
  TEST_ASSERT_EQUAL_UINT16(10, f.seq);
  TEST_ASSERT_EQUAL_UINT16(0, f.payloadLen);
  // Fixed-empty contract: any stowaway byte fails both gates.
  const uint8_t one[1] = {0x00};
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypePairDone, 1, one, 1, 0, 1, out, sizeof(out)));
  uint8_t wide[link::kOverhead + 1] = {0x4C, 0x07, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00};
  link::putU16BE(wide + 9, link::crc16Ccitt(wide, 9));
  TEST_ASSERT_FALSE(link::decodeFrame(wide, sizeof(wide), f));
  // Both new types are known; the next free type stays unknown.
  TEST_ASSERT_TRUE(link::isKnownType(link::kTypePairShow));
  TEST_ASSERT_TRUE(link::isKnownType(link::kTypePairDone));
  TEST_ASSERT_TRUE(link::isKnownType(0x06));
  TEST_ASSERT_TRUE(link::isKnownType(0x07));
  TEST_ASSERT_FALSE(link::isKnownType(0x08));
}
void test_link_phone_wire_headers_distinct() {
  // None of the frozen phone-wire headers may ever equal the link magic.
  static const uint8_t kFrozen[] = {0xAA, 0xBB, 0x50, 0x51, 0x52, 0xAC, 0xAD, 0x53, 0x54, 0x55, 0x56};
  for (size_t i = 0; i < sizeof(kFrozen); ++i) {
    TEST_ASSERT_NOT_EQUAL(link::kMagic, kFrozen[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(0x4C, link::kMagic);
  // Link type bytes are disjoint from the magic, so a resync scan cannot
  // mistake a type byte for a frame start.
  static const uint8_t kTypes[] = {link::kTypeFace,     link::kTypeOled,     link::kTypeCamCtl,
                                   link::kTypePairShow, link::kTypePairDone, link::kTypeJpeg,
                                   link::kTypeHb,       link::kTypeHbAck,    link::kTypeAck};
  for (size_t i = 0; i < sizeof(kTypes); ++i) {
    TEST_ASSERT_NOT_EQUAL(link::kMagic, kTypes[i]);
    TEST_ASSERT_TRUE(link::isKnownType(kTypes[i]));
  }
  TEST_ASSERT_FALSE(link::isKnownType(0x4C));
  TEST_ASSERT_FALSE(link::isKnownType(0xAA));
  TEST_ASSERT_FALSE(link::isKnownType(0x04));  // retired QRBITMAP: stays unknown
}
