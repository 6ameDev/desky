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

void test_link_qr_exact63_accept() {
  uint8_t bitmap[link::kQrBitmapLen];
  for (size_t i = 0; i < sizeof(bitmap); ++i) {
    bitmap[i] = static_cast<uint8_t>(i);
  }
  uint8_t out[link::kMaxFrame] = {};
  const size_t n = link::encodeFrame(link::kTypeQrBitmap, 7, bitmap, sizeof(bitmap), 0, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(link::kOverhead + link::kQrBitmapLen, n);
  link::Frame f;
  TEST_ASSERT_TRUE(link::decodeFrame(out, n, f));
  TEST_ASSERT_EQUAL_UINT8(link::kTypeQrBitmap, f.type);
  TEST_ASSERT_EQUAL_UINT16(7, f.seq);
  TEST_ASSERT_EQUAL_UINT16(link::kQrBitmapLen, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bitmap, f.payload, sizeof(bitmap));
}

void test_link_qr_rejects_62_and_64() {
  uint8_t bitmap[64];
  for (size_t i = 0; i < sizeof(bitmap); ++i) {
    bitmap[i] = static_cast<uint8_t>(i);
  }
  uint8_t out[link::kMaxFrame] = {};
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeQrBitmap, 1, bitmap, 62, 0, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, link::encodeFrame(link::kTypeQrBitmap, 1, bitmap, 64, 0, 1, out, sizeof(out)));
  // Hand-built 62B QR frame with a valid CRC must still fail the decode gate.
  uint8_t short62[link::kOverhead + 62];
  short62[0] = link::kMagic;
  short62[1] = link::kTypeQrBitmap;
  link::putU16BE(short62 + 2, 1);
  short62[4] = 0;
  short62[5] = 1;
  link::putU16BE(short62 + 6, 62);
  for (size_t i = 0; i < 62; ++i) {
    short62[link::kHeaderSize + i] = bitmap[i];
  }
  link::putU16BE(short62 + link::kHeaderSize + 62, link::crc16Ccitt(short62, link::kHeaderSize + 62));
  link::Frame f;
  TEST_ASSERT_FALSE(link::decodeFrame(short62, sizeof(short62), f));
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
      {link::kTypeFace, 1}, {link::kTypeOled, 1}, {link::kTypeCamCtl, 1}, {link::kTypeQrBitmap, 63},
      {link::kTypeJpeg, 7}, {link::kTypeHb, 10},  {link::kTypeHbAck, 5},  {link::kTypeAck, 2},
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

void test_link_phone_wire_headers_distinct() {
  // None of the frozen phone-wire headers may ever equal the link magic.
  static const uint8_t kFrozen[] = {0xAA, 0xBB, 0x50, 0x51, 0x52, 0xAC, 0xAD, 0x53, 0x54, 0x55, 0x56};
  for (size_t i = 0; i < sizeof(kFrozen); ++i) {
    TEST_ASSERT_NOT_EQUAL(link::kMagic, kFrozen[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(0x4C, link::kMagic);
  // Link type bytes are disjoint from the magic, so a resync scan cannot
  // mistake a type byte for a frame start.
  static const uint8_t kTypes[] = {link::kTypeFace, link::kTypeOled, link::kTypeCamCtl, link::kTypeQrBitmap,
                                   link::kTypeJpeg, link::kTypeHb,   link::kTypeHbAck,  link::kTypeAck};
  for (size_t i = 0; i < sizeof(kTypes); ++i) {
    TEST_ASSERT_NOT_EQUAL(link::kMagic, kTypes[i]);
    TEST_ASSERT_TRUE(link::isKnownType(kTypes[i]));
  }
  TEST_ASSERT_FALSE(link::isKnownType(0x4C));
  TEST_ASSERT_FALSE(link::isKnownType(0xAA));
}
