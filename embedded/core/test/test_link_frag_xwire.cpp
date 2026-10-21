// Link FRAG X-WIRE test (S/M): the REAL head producer (wifilink::fragSlice)
// composes the REAL codec frames (linkcode::encodeFrame, JPEG) which are
// concatenated and fed into the REAL core consumer (linkserver::pullFrame +
// linkcode::decodeFrame + linkcode::Reassembler).
//
// No synthetic fixtures on either side: slice geometry, frame bytes, stream
// framing and reassembly all run through the firmware functions. Any MTU /
// frag-index / frag-total / CRC / reassembly-order drift breaks this test.
//
// Sizes: 0 (empty rides one frame), 1, 1024 (one full frame), 1025 (split),
// 2500 (3 frags), 16384 (16 full frames, the firmware JPEG message cap).
// Plus out-of-order / duplicate / total-mismatch delivery proving
// reject-without-corruption. Run: pio test -e native-test.

#include <string.h>
#include <unity.h>

#include "../../head/src/transport/link_codec.h"
#include "../src/transport/link_server.h"

namespace {

// Static scratch (BSS idiom, like the firmware members — never stack/heap).
// 16384 is the firmware JPEG message cap (LinkServer::kJpegMsgCap,
// firmware-only constexpr, so the literal is pinned here and in geometry).
uint8_t s_msg[16384];
uint8_t s_stream[32768];
uint8_t s_reasm[16384];
uint8_t s_copy[3][linkcode::kMaxFrame];

const size_t kLens[] = {0, 1, 1024, 1025, 2500, 16384};
const size_t kNumLens = sizeof(kLens) / sizeof(kLens[0]);

// Deterministic non-trivial payload pattern.
void fillMsg(size_t len) {
  for (size_t i = 0; i < len; ++i) {
    s_msg[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
  }
}

// Producer seam: slice the message and encode every frag. Returns the total
// stream bytes appended at s_stream (streamLenOut).
size_t produceStream(size_t totalLen, uint16_t seqBase, size_t& streamLenOut) {
  fillMsg(totalLen);
  const size_t per = wifilink::perFramePayload();
  const size_t total = wifilink::fragTotalFor(totalLen);
  streamLenOut = 0;
  for (size_t idx = 0; idx < total; ++idx) {
    size_t off = 0;
    size_t chunk = 0;
    TEST_ASSERT_TRUE(
        wifilink::fragSlice(totalLen, per, static_cast<uint8_t>(idx), static_cast<uint8_t>(total), off, chunk));
    const uint8_t* ptr = (chunk > 0) ? (s_msg + off) : nullptr;
    const size_t n =
        linkcode::encodeFrame(linkcode::kTypeJpeg, static_cast<uint16_t>(seqBase + idx), ptr,
                              static_cast<uint16_t>(chunk), static_cast<uint8_t>(idx), static_cast<uint8_t>(total),
                              s_stream + streamLenOut, sizeof(s_stream) - streamLenOut);
    TEST_ASSERT_TRUE(n > 0);
    streamLenOut += n;
  }
  return total;
}

// Consumer seam: pull frames out of the stream, decode, push in order.
// Returns the reassembled length.
size_t consumeStream(size_t streamLen, linkcode::Reassembler& reasm, uint8_t* reasmBuf, size_t reasmCap) {
  reasm.reset(reasmBuf, reasmCap);
  size_t pos = 0;
  size_t expectIdx = 0;
  while (pos < streamLen) {
    size_t frameLen = 0;
    TEST_ASSERT_TRUE(linkserver::pullFrame(s_stream + pos, streamLen - pos, frameLen) == linkserver::Pull::kHaveFrame);
    linkcode::Frame f;
    TEST_ASSERT_TRUE(linkcode::decodeFrame(s_stream + pos, frameLen, f));
    TEST_ASSERT_EQUAL_UINT8(linkcode::kTypeJpeg, f.type);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expectIdx), f.fragIdx);
    TEST_ASSERT_TRUE(reasm.push(f.fragIdx, f.fragTotal, f.payload, f.payloadLen));
    ++expectIdx;
    pos += frameLen;
  }
  TEST_ASSERT_EQUAL_size_t(streamLen, pos);
  return reasm.len();
}

}  // namespace

// Slice geometry is pinned: per-frame payload, frag totals per size, and the
// literal offsets/lengths of the 2500B split (1024/1024/452).
void test_link_frag_xwire_geometry() {
  TEST_ASSERT_EQUAL_size_t(1024, wifilink::perFramePayload());
  TEST_ASSERT_EQUAL_size_t(1, wifilink::fragTotalFor(0));
  TEST_ASSERT_EQUAL_size_t(1, wifilink::fragTotalFor(1));
  TEST_ASSERT_EQUAL_size_t(1, wifilink::fragTotalFor(1024));
  TEST_ASSERT_EQUAL_size_t(2, wifilink::fragTotalFor(1025));
  TEST_ASSERT_EQUAL_size_t(3, wifilink::fragTotalFor(2500));
  TEST_ASSERT_EQUAL_size_t(16, wifilink::fragTotalFor(16384));
  size_t off = 0;
  size_t len = 0;
  TEST_ASSERT_TRUE(wifilink::fragSlice(2500, 1024, 0, 3, off, len));
  TEST_ASSERT_EQUAL_size_t(0, off);
  TEST_ASSERT_EQUAL_size_t(1024, len);
  TEST_ASSERT_TRUE(wifilink::fragSlice(2500, 1024, 1, 3, off, len));
  TEST_ASSERT_EQUAL_size_t(1024, off);
  TEST_ASSERT_EQUAL_size_t(1024, len);
  TEST_ASSERT_TRUE(wifilink::fragSlice(2500, 1024, 2, 3, off, len));
  TEST_ASSERT_EQUAL_size_t(2048, off);
  TEST_ASSERT_EQUAL_size_t(452, len);
  TEST_ASSERT_FALSE(wifilink::fragSlice(2500, 1024, 3, 3, off, len));  // idx >= total
  TEST_ASSERT_TRUE(wifilink::fragSlice(0, 1024, 0, 1, off, len));      // empty rides one frame
  TEST_ASSERT_EQUAL_size_t(0, len);
}

// Full seam per size: slice -> encode -> concat -> pull -> decode -> push in
// order; the reassembled message must be complete and byte-identical.
void test_link_frag_xwire_round_trip_all_sizes() {
  for (size_t li = 0; li < kNumLens; ++li) {
    const size_t totalLen = kLens[li];
    size_t streamLen = 0;
    produceStream(totalLen, 0x2000, streamLen);
    linkcode::Reassembler reasm;
    const size_t gotLen = consumeStream(streamLen, reasm, s_reasm, sizeof(s_reasm));
    TEST_ASSERT_TRUE(reasm.complete());
    TEST_ASSERT_EQUAL_size_t(totalLen, gotLen);
    if (totalLen > 0) {
      TEST_ASSERT_EQUAL_UINT8_ARRAY(s_msg, s_reasm, totalLen);
    }
  }
}

// Disorder delivery: out-of-order / duplicate / total-mismatch frags must be
// rejected WITHOUT mutating state; the in-order tail then still completes
// byte-identical. Uses the 2500B message (3 frags: 1024/1024/452).
void test_link_frag_xwire_rejects_disorder_without_corruption() {
  size_t streamLen = 0;
  const size_t total = produceStream(2500, 0x3000, streamLen);
  TEST_ASSERT_EQUAL_size_t(3, total);
  // Decode each frame into a retained copy (decode borrows the stream).
  size_t pos = 0;
  linkcode::Frame f[3];
  for (size_t i = 0; i < 3; ++i) {
    size_t frameLen = 0;
    TEST_ASSERT_TRUE(linkserver::pullFrame(s_stream + pos, streamLen - pos, frameLen) == linkserver::Pull::kHaveFrame);
    for (size_t k = 0; k < frameLen; ++k) {
      s_copy[i][k] = s_stream[pos + k];
    }
    TEST_ASSERT_TRUE(linkcode::decodeFrame(s_copy[i], frameLen, f[i]));
    pos += frameLen;
  }
  linkcode::Reassembler reasm;
  reasm.reset(s_reasm, sizeof(s_reasm));
  // Gap first: frag 1 before frag 0 -> reject, nothing latched.
  TEST_ASSERT_FALSE(reasm.push(f[1].fragIdx, f[1].fragTotal, f[1].payload, f[1].payloadLen));
  TEST_ASSERT_FALSE(reasm.complete());
  TEST_ASSERT_EQUAL_size_t(0, reasm.len());
  TEST_ASSERT_EQUAL_UINT8(0, reasm.next());
  // Frag 0 accepted; an immediate duplicate is rejected without overwrite.
  TEST_ASSERT_TRUE(reasm.push(f[0].fragIdx, f[0].fragTotal, f[0].payload, f[0].payloadLen));
  TEST_ASSERT_EQUAL_size_t(1024, reasm.len());
  TEST_ASSERT_FALSE(reasm.push(f[0].fragIdx, f[0].fragTotal, f[0].payload, f[0].payloadLen));
  TEST_ASSERT_EQUAL_size_t(1024, reasm.len());
  TEST_ASSERT_EQUAL_UINT8(1, reasm.next());
  // Tail before its predecessor (gap) -> reject.
  TEST_ASSERT_FALSE(reasm.push(f[2].fragIdx, f[2].fragTotal, f[2].payload, f[2].payloadLen));
  TEST_ASSERT_EQUAL_size_t(1024, reasm.len());
  // Same idx, different total -> reject (total mismatch).
  TEST_ASSERT_FALSE(reasm.push(f[1].fragIdx, 2, f[1].payload, f[1].payloadLen));
  TEST_ASSERT_EQUAL_size_t(1024, reasm.len());
  // In-order tail completes byte-identical.
  TEST_ASSERT_TRUE(reasm.push(f[1].fragIdx, f[1].fragTotal, f[1].payload, f[1].payloadLen));
  TEST_ASSERT_TRUE(reasm.push(f[2].fragIdx, f[2].fragTotal, f[2].payload, f[2].payloadLen));
  TEST_ASSERT_TRUE(reasm.complete());
  TEST_ASSERT_EQUAL_size_t(2500, reasm.len());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_msg, s_reasm, 2500);
}
