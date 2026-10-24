#pragma once
// Shared expression catalog: one V1 face table both firmwares compile from.
// Header-only, Arduino-free, no heap, no logging. Normative source: this file.
// BUMP RULE (additive only): a new expression takes the next free id. Never
// renumber, never reorder, never change a shipped row.

#include <stddef.h>
#include <stdint.h>

namespace exprcat {

// Ids mirror the head FACE_* vocabulary exactly; nothing renumbers.
constexpr uint8_t kBoot = 0;
constexpr uint8_t kHappy = 1;
constexpr uint8_t kSad = 2;
constexpr uint8_t kBlink = 3;
constexpr uint8_t kAlert = 4;
// Id 5 is QR: entered only via showQr, never via a face id, so it has no row.
constexpr uint8_t kCurious = 6;
constexpr uint8_t kSurprised = 7;
constexpr uint8_t kCount = 8;

// Reserved media placeholders: no sound/motion vocabulary ships yet, so every
// row carries NONE. 0xFF is frozen as the NONE sentinel, never a valid id.
constexpr uint8_t kSoundNone = 0xFF;
constexpr uint8_t kMotionNone = 0xFF;

constexpr uint32_t kSurprisedHoldMs = 1500;

struct Entry {
  uint8_t id;
  const char* name;
  uint8_t priority;
  bool playOnce;
  uint32_t holdMs;
  uint8_t eye;     // Opaque to core; head interprets.
  uint8_t sound;   // Reserved, always kSoundNone.
  uint8_t motion;  // Reserved, always kMotionNone.
};

constexpr Entry kTable[] = {
    {kBoot, "boot", 0, false, 0, 0, kSoundNone, kMotionNone},
    {kHappy, "happy", 1, false, 0, 3, kSoundNone, kMotionNone},
    {kSad, "sad", 3, false, 0, 4, kSoundNone, kMotionNone},
    {kBlink, "blink", 0, false, 0, 0, kSoundNone, kMotionNone},
    {kAlert, "alert", 4, false, 0, 8, kSoundNone, kMotionNone},
    {kCurious, "curious", 2, false, 0, 17, kSoundNone, kMotionNone},
    {kSurprised, "surprised", 5, true, kSurprisedHoldMs, 8, kSoundNone, kMotionNone},
};

constexpr size_t kEntryCount = sizeof(kTable) / sizeof(kTable[0]);

inline size_t count() { return kEntryCount; }

// Fail-closed: unknown ids (including QR) yield nullptr.
inline const Entry* lookup(uint8_t id) {
  for (size_t i = 0; i < kEntryCount; ++i) {
    if (kTable[i].id == id) {
      return &kTable[i];
    }
  }
  return nullptr;
}

inline uint8_t priorityFor(uint8_t id) {
  const Entry* e = lookup(id);
  return e == nullptr ? 0 : e->priority;
}

inline bool isPlayOnce(uint8_t id) {
  const Entry* e = lookup(id);
  return e != nullptr && e->playOnce;
}

inline uint32_t holdMsFor(uint8_t id) {
  const Entry* e = lookup(id);
  return e == nullptr ? 0 : e->holdMs;
}

// Neutral family: requests for these always release the active id, from any
// state (both managers bypass the priority check for them).
inline bool isNeutral(uint8_t id) { return id == kBoot || id == kBlink; }

}  // namespace exprcat
