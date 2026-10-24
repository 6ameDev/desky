#pragma once
// Shared expression catalog: one V2 face+media table both firmwares compile from.
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
constexpr uint8_t kAngry = 8;
constexpr uint8_t kScared = 9;
constexpr uint8_t kExcited = 10;
constexpr uint8_t kConfused = 11;
constexpr uint8_t kCount = 12;

// V1 rows predate the media vocabularies, so every media/pose field on those
// rows carries NONE. 0xFF is frozen as the NONE sentinel, never a valid id.
constexpr uint8_t kSoundNone = 0xFF;
constexpr uint8_t kMotionNone = 0xFF;
constexpr uint8_t kNeckNone = 0xFF;
constexpr uint8_t kArmNone = 0xFF;

// V2 media/pose vocabularies: small ids per new row; NONE stays 0xFF.
constexpr uint8_t kSoundAngry = 0;
constexpr uint8_t kSoundScared = 1;
constexpr uint8_t kSoundExcited = 2;
constexpr uint8_t kSoundConfused = 3;
constexpr uint8_t kMotionShake = 0;
constexpr uint8_t kMotionCower = 1;
constexpr uint8_t kMotionBounce = 2;
constexpr uint8_t kMotionTilt = 3;
constexpr uint8_t kNeckAngry = 0;
constexpr uint8_t kNeckScared = 1;
constexpr uint8_t kNeckExcited = 2;
constexpr uint8_t kNeckConfused = 3;
constexpr uint8_t kArmAngry = 0;
constexpr uint8_t kArmScared = 1;
constexpr uint8_t kArmExcited = 2;
constexpr uint8_t kArmConfused = 3;

constexpr uint32_t kSurprisedHoldMs = 1500;

struct Entry {
  uint8_t id;
  const char* name;
  uint8_t priority;
  bool playOnce;
  uint32_t holdMs;
  uint8_t eye;     // Opaque to core; head interprets.
  uint8_t sound;   // kSoundNone when the row carries no audio cue.
  uint8_t motion;  // kMotionNone when the row carries no motion primitive.
  uint8_t neckPose;  // kNeckNone when the row carries no neck pose.
  uint8_t armPose;   // kArmNone when the row carries no arm gesture.
};

constexpr Entry kTable[] = {
    {kBoot, "boot", 0, false, 0, 0, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kHappy, "happy", 1, false, 0, 3, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kSad, "sad", 3, false, 0, 4, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kBlink, "blink", 0, false, 0, 0, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kAlert, "alert", 4, false, 0, 8, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kCurious, "curious", 2, false, 0, 17, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kSurprised, "surprised", 5, true, kSurprisedHoldMs, 8, kSoundNone, kMotionNone, kNeckNone, kArmNone},
    {kAngry, "angry", 8, false, 0, 1, kSoundAngry, kMotionShake, kNeckAngry, kArmAngry},
    {kScared, "scared", 9, false, 0, 16, kSoundScared, kMotionCower, kNeckScared, kArmScared},
    {kExcited, "excited", 7, false, 0, 2, kSoundExcited, kMotionBounce, kNeckExcited, kArmExcited},
    {kConfused, "confused", 6, false, 0, 9, kSoundConfused, kMotionTilt, kNeckConfused, kArmConfused},
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
