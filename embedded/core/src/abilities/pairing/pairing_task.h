#pragma once
// desky v2 pairing task binding — abilities/pairing (firmware-only crypto).
//
// mbedTLS glue (deriveKek/wrapPsk/ctEqual/deriveRekeyPsk) called by the
// transport-embedded handlers in udp_server.h. Slice 6b moves those handlers
// (handlePairReq/Unbond/Rekey/Alive/Disconnect, LastPair cache, QR staging)
// here, leaving transport as dumb pipes. Never included by host tests.

#include "pairing_codec.h"
#include "pairing_manager.h"

#ifdef ARDUINO
// ── Firmware: pairing crypto glue (mbedTLS, Task 4) ───────────────────────
// Arduino-ESP32 (S3) bundles mbedTLS; headers live under the framework
// package and are included as <mbedtls/...>. No heap, no String, and no
// logging of key material anywhere in this section (never LOG key, nonce,
// or code bytes) — failures surface as silent bools for the PAIR_ERR path.
//
// CALL-ORDER CONTRACT (store-before-send; the PAIR_OK send path must follow
// it exactly so app-bonded/robot-unbonded is impossible):
//   1. Core::tryCode(in, len, nowMs) == Result::kOk (code verified + burned
//      inside Core; single-use, window/lockout enforced by the top half).
//   2. Fresh PSK = 16 esp_random bytes into a caller-owned stack buffer.
//   3. ConfigStore::saveBond(psk, 1) FIRST — persist before anything leaves
//      the radio. A failed save aborts: answer PAIR_ERR, never PAIR_OK.
//   4. deriveKek(code, clientNonce, serverNonce, kek), then
//      wrapPsk(psk, kek, clientNonce, serverNonce, persistU8, ct, tag)
//      with persistU8 = kPairPersistDefault (128) — sanitized inside wrap.
//   5. Send PAIR_OK ([0x51 | server_nonce | ct17 | tag | XOR]); zeroize the
//      kek/psk stack copies after the send.
// Layer rule: crypto glue is abilities/pairing (this header); NVS persistence
// is services/ConfigStore; policy/state is pairing::Core (pairing_manager.h).

#include <mbedtls/cipher.h>
// NOTE: this vendored constant_time.h ships WITHOUT the extern "C" guard
// every other mbedtls header has, so a plain include mangles
// mbedtls_ct_memcmp in C++ and ctEqual below fails at link (undefined
// _Z17mbedtls_ct_memcmp...). The prebuilt lib provides the C symbol, hence
// the explicit C-linkage wrapper (harmless if a future header adds its own).
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include "common/fault_manager.h"  // DESKY_ASSERT (firmware side only)

namespace pairing {

// Derives the pairing KEK: KEK = HKDF-SHA256(salt=client_nonce||server_nonce,
// IKM=6 ASCII code digits, info="desky-pair-v1", L=16). Returns false on null
// input or any mbedTLS error (kekOut zeroized on failure). Salt lives in a
// fixed-size stack buffer and is zeroized on all paths.
inline bool deriveKek(const uint8_t code6[kCodeLen], const uint8_t clientNonce[kPairNonceLen],
                      const uint8_t serverNonce[kPairNonceLen], uint8_t kekOut[kKekLen]) {
  DESKY_ASSERT(code6 != nullptr);
  DESKY_ASSERT(clientNonce != nullptr);
  DESKY_ASSERT(serverNonce != nullptr);
  DESKY_ASSERT(kekOut != nullptr);
  if (code6 == nullptr || clientNonce == nullptr || serverNonce == nullptr || kekOut == nullptr) {
    return false;
  }
  uint8_t salt[kPairNonceLen * 2];
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    salt[i] = clientNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    salt[kPairNonceLen + i] = serverNonce[i];
  }
  static const char kInfo[] = "desky-pair-v1";
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = -1;
  if (md != nullptr) {
    rc = mbedtls_hkdf(md, salt, sizeof(salt), code6, kCodeLen, reinterpret_cast<const unsigned char*>(kInfo),
                      sizeof(kInfo) - 1, kekOut, kKekLen);
  }
  mbedtls_platform_zeroize(salt, sizeof(salt));
  if (rc != 0) {
    mbedtls_platform_zeroize(kekOut, kKekLen);
    return false;
  }
  return true;
}

// Wraps the bonded PSK + persist-interval byte for PAIR_OK:
// ct||tag = AES-128-GCM(KEK, nonce12=server_nonce||client_nonce||00 00 00
// 00, AAD=0x51||client_nonce||server_nonce, psk16||persist_u8). The 17th
// plaintext byte carries sanitizePersistEvery(persistU8) (default 128), so
// the cadence is authenticated by the GCM tag+AAD exactly like the PSK.
// Layout: PAIR_OK 39B [0x51|srvNonce4|ct17|tag16|XOR] (see
// shared/protocol/README.md §v1.1 addendum). Returns false on null input
// or any mbedTLS error; gcm_free runs on all paths, and ct/tag outputs are
// zeroized on failure (caller still owns psk/kek — const here — and must
// zeroize its own copies after the send per the contract above).
inline bool wrapPsk(const uint8_t psk[kPskLen], const uint8_t kek[kKekLen], const uint8_t clientNonce[kPairNonceLen],
                    const uint8_t serverNonce[kPairNonceLen], uint8_t persistU8, uint8_t ctOut[kPairCtLen],
                    uint8_t tagOut[kPairTagLen]) {
  DESKY_ASSERT(psk != nullptr);
  DESKY_ASSERT(kek != nullptr);
  DESKY_ASSERT(clientNonce != nullptr);
  DESKY_ASSERT(serverNonce != nullptr);
  DESKY_ASSERT(ctOut != nullptr);
  DESKY_ASSERT(tagOut != nullptr);
  if (psk == nullptr || kek == nullptr || clientNonce == nullptr || serverNonce == nullptr || ctOut == nullptr ||
      tagOut == nullptr) {
    return false;
  }
  uint8_t iv[kPairIvLen];
  uint8_t aad[kPairAadLen];
  uint8_t plain[kPairWrapPlainLen];
  for (size_t i = 0; i < kPskLen; ++i) {
    plain[i] = psk[i];
  }
  plain[kPskLen] = sanitizePersistEvery(persistU8);
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    iv[i] = serverNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    iv[kPairNonceLen + i] = clientNonce[i];
  }
  for (size_t i = 2 * kPairNonceLen; i < kPairIvLen; ++i) {
    iv[i] = 0x00;
  }
  aad[0] = kPairOkHeader;
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    aad[1 + i] = clientNonce[i];
  }
  for (size_t i = 0; i < kPairNonceLen; ++i) {
    aad[1 + kPairNonceLen + i] = serverNonce[i];
  }
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, kek, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, kPairWrapPlainLen, iv, sizeof(iv), aad, sizeof(aad),
                                   plain, ctOut, kPairTagLen, tagOut);
  }
  mbedtls_gcm_free(&ctx);
  mbedtls_platform_zeroize(iv, sizeof(iv));
  mbedtls_platform_zeroize(aad, sizeof(aad));
  mbedtls_platform_zeroize(plain, sizeof(plain));
  if (rc != 0) {
    mbedtls_platform_zeroize(ctOut, kPairCtLen);
    mbedtls_platform_zeroize(tagOut, kPairTagLen);
    return false;
  }
  return true;
}

// Constant-time buffer compare for the Task 5 DRIVE_V2 verify path
// (HMAC tag check). Thin wrapper over mbedtls_ct_memcmp; returns false on
// null input. Note: pairing::constantTimeEqual6 above stays the host-testable
// semantic for the 6-byte code compare — same no-early-exit rule, Arduino-free
// so Unity pins it; this wrapper is just the firmware call-site rule.
inline bool ctEqual(const void* a, const void* b, size_t n) {
  DESKY_ASSERT(a != nullptr);
  DESKY_ASSERT(b != nullptr);
  if (a == nullptr || b == nullptr) {
    return false;
  }
  return mbedtls_ct_memcmp(a, b, n) == 0;
}

// Tier-1 rekey key schedule (firmware side):
//   PSK_next = HKDF-SHA256(salt="desky-rekey-v1", IKM=oldPsk16,
//                           info=eraBE2(new), L=16).
// Salt lives in ROM (string literal, no stack copy needed); the 2-byte era
// info is built on the stack and zeroized. Returns false on null input or
// any mbedTLS error (out zeroized on failure). Caller owns oldPsk/newPsk
// lifetime and must zeroize its own copies after the switch per the
// store-before-ACK contract (see udp_server.h handleRekey).
inline bool deriveRekeyPsk(const uint8_t oldPsk[kPskLen], uint16_t nextEra, uint8_t nextPskOut[kPskLen]) {
  DESKY_ASSERT(oldPsk != nullptr);
  DESKY_ASSERT(nextPskOut != nullptr);
  if (oldPsk == nullptr || nextPskOut == nullptr) {
    return false;
  }
  static const char kSalt[] = "desky-rekey-v1";
  uint8_t info[2];
  info[0] = static_cast<uint8_t>((nextEra >> 8) & 0xFFu);
  info[1] = static_cast<uint8_t>(nextEra & 0xFFu);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = -1;
  if (md != nullptr) {
    rc = mbedtls_hkdf(md, reinterpret_cast<const unsigned char*>(kSalt), sizeof(kSalt) - 1, oldPsk, kPskLen, info,
                      sizeof(info), nextPskOut, kPskLen);
  }
  mbedtls_platform_zeroize(info, sizeof(info));
  if (rc != 0) {
    mbedtls_platform_zeroize(nextPskOut, kPskLen);
    return false;
  }
  return true;
}

}  // namespace pairing

#endif  // ARDUINO
