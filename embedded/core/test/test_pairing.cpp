// Host unit tests for the pairing core (no hardware).
// Run: pio test -e native-test
//
// Covers Task 2: golden accept + burn-once, bad-code tries, 5th-try lockout
// with 30s expiry, 5-min window expiry, pre-window attempts, malformed
// input. Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/middleware/pairing.h"

namespace {

const uint8_t kCode[pairing::kCodeLen] = {'1', '2', '3', '4', '5', '6'};
const uint8_t kWrong[pairing::kCodeLen] = {'6', '5', '4', '3', '2', '1'};

}  // namespace

void test_pair_golden_accept_burns_once() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(1000, kCode));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kOk),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 1001)));
  TEST_ASSERT_TRUE(core.bonded());
  // Same correct code replayed post-bond: never OK twice.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kAlreadyBonded),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 1002)));
}

void test_pair_bad_code_counts_tries() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 10)));
  TEST_ASSERT_EQUAL_UINT32(1, core.tries());
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 11)));
  TEST_ASSERT_EQUAL_UINT32(2, core.tries());
  TEST_ASSERT_FALSE(core.bonded());
}

void test_pair_fifth_try_locks_then_expires() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  for (uint32_t i = 0; i < pairing::kMaxTries - 1; ++i) {
    TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                      static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 10 + i)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kLocked),
                    static_cast<int>(core.tryCode(kWrong, pairing::kCodeLen, 20)));
  // Still locked just before 30s.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kLocked),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 20 + pairing::kLockoutMs - 1)));
  // After lockout the correct code works in the same window.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kOk),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 20 + pairing::kLockoutMs)));
}

void test_pair_window_expiry_rejects() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kNotInWindow),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, pairing::kWindowMs + 1)));
}

void test_pair_pre_window_rejects() {
  pairing::Core core;
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kNotInWindow),
                    static_cast<int>(core.tryCode(kCode, pairing::kCodeLen, 0)));
}

void test_pair_malformed_input_is_bad_code() {
  pairing::Core core;
  TEST_ASSERT_TRUE(core.openWindow(0, kCode));
  const uint8_t shortCode[4] = {'1', '2', '3', '4'};
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(shortCode, sizeof(shortCode), 10)));
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(nullptr, pairing::kCodeLen, 11)));
  const uint8_t nonDigit[pairing::kCodeLen] = {'1', '2', '3', '4', '5', 'X'};
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kBadCode),
                    static_cast<int>(core.tryCode(nonDigit, pairing::kCodeLen, 12)));
  TEST_ASSERT_FALSE(core.bonded());
}

// v1.1 persist-interval byte: minimal guard (not an allowlist) — 8..255
// pass, 0..7 (incl. div-zero 0) and anything above the u8 wire range
// (incl. the app-reserved 256..1024) fall back to 128.
void test_pair_persist_every_guard() {
  TEST_ASSERT_EQUAL_UINT8(128, pairing::sanitizePersistEvery(0));
  TEST_ASSERT_EQUAL_UINT8(128, pairing::sanitizePersistEvery(7));
  TEST_ASSERT_EQUAL_UINT8(8, pairing::sanitizePersistEvery(8));
  TEST_ASSERT_EQUAL_UINT8(16, pairing::sanitizePersistEvery(16));
  TEST_ASSERT_EQUAL_UINT8(128, pairing::sanitizePersistEvery(128));
  TEST_ASSERT_EQUAL_UINT8(255, pairing::sanitizePersistEvery(255));
  TEST_ASSERT_EQUAL_UINT8(128, pairing::sanitizePersistEvery(256));
  TEST_ASSERT_EQUAL_UINT8(128, pairing::sanitizePersistEvery(1024));
  TEST_ASSERT_EQUAL_UINT8(128, pairing::sanitizePersistEvery(65535));
}

// v1.1 wire layout: GCM plaintext psk16||persist_u8 (17B) -> PAIR_OK 39B;
// legacy v1 robots send 38B with no byte (app defaults to 128).
void test_pair_wrap_layout_consts() {
  TEST_ASSERT_EQUAL_UINT8(128, pairing::kPairPersistDefault);
  TEST_ASSERT_EQUAL(17, static_cast<int>(pairing::kPairWrapPlainLen));
  TEST_ASSERT_EQUAL(17, static_cast<int>(pairing::kPairCtLen));
  TEST_ASSERT_EQUAL(39, static_cast<int>(pairing::kPairOkSize));
  TEST_ASSERT_EQUAL(38, static_cast<int>(pairing::kPairOkLegacySize));
}

// Tier-1 silent auto-rekey: wire constants + tag-message goldens. The HKDF
// key schedule (deriveRekeyPsk) is ARDUINO-only (mbedTLS) and exercised on
// hardware; the cross-side vectors live app-side (PairingCryptoTest rekey
// goldens: nextPsk=df7441d0... + REQ/ACK tags) — these host tests pin the
// message layouts both sides must agree on.
void test_rekey_wire_consts() {
  TEST_ASSERT_EQUAL_UINT8(0x53, pairing::kRekeyReqHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(pairing::kRekeyReqSize));
  TEST_ASSERT_EQUAL_UINT8(0x54, pairing::kRekeyAckHeader);
  TEST_ASSERT_EQUAL(10, static_cast<int>(pairing::kRekeyAckSize));
  TEST_ASSERT_EQUAL(6, static_cast<int>(pairing::kRekeyTagLen));
  TEST_ASSERT_EQUAL(11, static_cast<int>(pairing::kRekeyMsgLen));
  TEST_ASSERT_EQUAL(10, static_cast<int>(pairing::kRekeyAckMsgLen));
}

void test_rekey_tag_msg_golden() {
  uint8_t msg[pairing::kRekeyMsgLen];
  pairing::rekeyTagMsg(1u, 100u, msg);
  // "REKEY" || eraBE2(0001) || counterBE4(00000064).
  const uint8_t expected[11] = {0x52, 0x45, 0x4B, 0x45, 0x59, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
  // Era/counter lay out big-endian.
  pairing::rekeyTagMsg(0x0102u, 0x01020304u, msg);
  const uint8_t be[11] = {0x52, 0x45, 0x4B, 0x45, 0x59, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(be, msg, sizeof(be));
  // Null-safe (no crash on null out).
  pairing::rekeyTagMsg(1u, 1u, nullptr);
}

void test_rekey_ack_tag_msg_golden() {
  uint8_t msg[pairing::kRekeyAckMsgLen];
  pairing::rekeyAckTagMsg(1u, msg);
  // "REKEYACK" || eraBE2(0001).
  const uint8_t expected[10] = {0x52, 0x45, 0x4B, 0x45, 0x59, 0x41, 0x43, 0x4B, 0x00, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
  pairing::rekeyAckTagMsg(0x0102u, msg);
  TEST_ASSERT_EQUAL_UINT8(0x01, msg[8]);
  TEST_ASSERT_EQUAL_UINT8(0x02, msg[9]);
  pairing::rekeyAckTagMsg(1u, nullptr);
}

// ALIVE + DISCONNECT (robot alive rule): domain constants pin the claimed
// headers/sizes (0x55/0x56 — never 0xAA/0xAC/0xBB/0x50/0x51/0x52/0x53/0x54/
// 0xAD) and the tag-message lengths. HMAC tags themselves are ARDUINO-only
// (mbedTLS) and pinned as cross-side vectors in shared/protocol/README.md
// (alive_tag=dd152baa54f7, disconnect_tag=ca90ca039040 for era=1,
// counter=100 under the shared psk) — these host tests pin the message
// layouts both sides must agree on.
void test_alive_disconnect_wire_consts() {
  TEST_ASSERT_EQUAL_UINT8(0x55, pairing::kAliveHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(pairing::kAliveSize));
  TEST_ASSERT_EQUAL_UINT8(0x56, pairing::kDisconnectHeader);
  TEST_ASSERT_EQUAL(14, static_cast<int>(pairing::kDisconnectSize));
  TEST_ASSERT_EQUAL(6, static_cast<int>(pairing::kAliveTagLen));
  TEST_ASSERT_EQUAL(11, static_cast<int>(pairing::kAliveMsgLen));
  TEST_ASSERT_EQUAL(6, static_cast<int>(pairing::kDisconnectTagLen));
  TEST_ASSERT_EQUAL(16, static_cast<int>(pairing::kDisconnectMsgLen));
}

void test_alive_tag_msg_golden() {
  uint8_t msg[pairing::kAliveMsgLen];
  pairing::aliveTagMsg(1u, 100u, msg);
  // "ALIVE" || eraBE2(0001) || counterBE4(00000064).
  const uint8_t expected[11] = {0x41, 0x4C, 0x49, 0x56, 0x45, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
  // Era/counter lay out big-endian.
  pairing::aliveTagMsg(0x0102u, 0x01020304u, msg);
  const uint8_t be[11] = {0x41, 0x4C, 0x49, 0x56, 0x45, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(be, msg, sizeof(be));
  // Null-safe (no crash on null out).
  pairing::aliveTagMsg(1u, 1u, nullptr);
}

void test_disconnect_tag_msg_golden() {
  uint8_t msg[pairing::kDisconnectMsgLen];
  pairing::disconnectTagMsg(1u, 100u, msg);
  // "DISCONNECT" || eraBE2(0001) || counterBE4(00000064).
  const uint8_t expected[16] = {0x44, 0x49, 0x53, 0x43, 0x4F, 0x4E, 0x4E, 0x45,
                                0x43, 0x54, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, msg, sizeof(expected));
  // Era/counter lay out big-endian.
  pairing::disconnectTagMsg(0x0102u, 0x01020304u, msg);
  const uint8_t be[16] = {0x44, 0x49, 0x53, 0x43, 0x4F, 0x4E, 0x4E, 0x45,
                          0x43, 0x54, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(be, msg, sizeof(be));
  // Null-safe (no crash on null out).
  pairing::disconnectTagMsg(1u, 1u, nullptr);
}

// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the pairing tests are declared extern there.
