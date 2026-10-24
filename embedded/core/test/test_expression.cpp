// Host unit tests for the core expression owner (no hardware).
// Run: pio test -e native-test
//
// Covers the codec id mirror + neutral predicate, the manager one-active
// policy (priority order, preemption, lower-never-displaces, neutral release,
// yield-to-neutral, play-once expiry/retrigger with fake millis, wrap
// safety, fail-closed unknown ids), the task staged-dispatch latch, and the
// bundle conductor (per-row cue+primitive mapping, NONE silent/still,
// cancel-then-start on preemption/release/yield, retrigger restart, rejected
// silence, link-retry no-replay) over fake audio/motion sinks plus the
// Arduino-free cue/primitive tables.
// Runner lives in test_udp_codec.cpp (single main).

#include <unity.h>

#include "../src/abilities/expression/expression_codec.h"
#include "../src/abilities/expression/expression_manager.h"
#include "../src/abilities/expression/expression_task.h"

void test_exprcodec_ids_mirror_catalog() {
  TEST_ASSERT_EQUAL_UINT8(exprcat::kBoot, expression::kBoot);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kHappy, expression::kHappy);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSad, expression::kSad);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kBlink, expression::kBlink);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kAlert, expression::kAlert);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kCurious, expression::kCurious);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kSurprised, expression::kSurprised);
  TEST_ASSERT_EQUAL_UINT8(exprcat::kCount, expression::kCount);
}

void test_exprcat_neutral_family() {
  TEST_ASSERT_TRUE(exprcat::isNeutral(exprcat::kBoot));
  TEST_ASSERT_TRUE(exprcat::isNeutral(exprcat::kBlink));
  TEST_ASSERT_FALSE(exprcat::isNeutral(exprcat::kHappy));
  TEST_ASSERT_FALSE(exprcat::isNeutral(exprcat::kCurious));
  TEST_ASSERT_FALSE(exprcat::isNeutral(exprcat::kSad));
  TEST_ASSERT_FALSE(exprcat::isNeutral(exprcat::kAlert));
  TEST_ASSERT_FALSE(exprcat::isNeutral(exprcat::kSurprised));
  TEST_ASSERT_FALSE(exprcat::isNeutral(5));
  TEST_ASSERT_FALSE(exprcat::isNeutral(0xFF));
}

void test_exprmgr_starts_neutral() {
  expression::Manager mgr;
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
}

void test_exprmgr_priority_order() {
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kBoot) < exprcat::priorityFor(expression::kHappy));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kHappy) < exprcat::priorityFor(expression::kCurious));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kCurious) < exprcat::priorityFor(expression::kSad));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kSad) < exprcat::priorityFor(expression::kAlert));
  TEST_ASSERT_TRUE(exprcat::priorityFor(expression::kAlert) < exprcat::priorityFor(expression::kSurprised));
}

void test_exprmgr_higher_preempts_lower() {
  expression::Manager mgr;
  const uint8_t chain[] = {expression::kHappy, expression::kCurious, expression::kSad, expression::kAlert,
                           expression::kSurprised};
  for (size_t i = 0; i < sizeof(chain); ++i) {
    TEST_ASSERT_TRUE(mgr.request(chain[i], 0));
    TEST_ASSERT_EQUAL_UINT8(chain[i], mgr.active());
  }
}

void test_exprmgr_lower_never_displaces_higher() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 0));
  const uint8_t lower[] = {expression::kHappy, expression::kCurious, expression::kSad, expression::kAlert};
  for (size_t i = 0; i < sizeof(lower); ++i) {
    TEST_ASSERT_FALSE(mgr.request(lower[i], 100));
    TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, mgr.active());
  }
  TEST_ASSERT_TRUE(mgr.request(expression::kBoot, 150));
  TEST_ASSERT_TRUE(mgr.request(expression::kAlert, 200));
  TEST_ASSERT_FALSE(mgr.request(expression::kHappy, 300));
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, mgr.active());
}

void test_exprmgr_neutral_releases_from_any() {
  expression::Manager mgr;
  const uint8_t moods[] = {expression::kHappy, expression::kCurious, expression::kSad, expression::kAlert,
                           expression::kSurprised};
  for (size_t i = 0; i < sizeof(moods); ++i) {
    TEST_ASSERT_TRUE(mgr.request(moods[i], 0));
    TEST_ASSERT_TRUE(mgr.request(expression::kBoot, 10));
    TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  }
  TEST_ASSERT_TRUE(mgr.request(expression::kSad, 20));
  TEST_ASSERT_TRUE(mgr.request(expression::kBlink, 30));
  TEST_ASSERT_EQUAL_UINT8(expression::kBlink, mgr.active());
}

void test_exprmgr_yields_neutral_not_previous_mood() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kHappy, 0));
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 10));
  TEST_ASSERT_TRUE(mgr.tick(10 + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  TEST_ASSERT_TRUE(mgr.request(expression::kHappy, 2000));
  TEST_ASSERT_TRUE(mgr.request(expression::kSad, 2010));
  TEST_ASSERT_TRUE(mgr.request(expression::kBoot, 2020));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
}

void test_exprmgr_play_once_expiry() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 1000));
  TEST_ASSERT_FALSE(mgr.tick(1000 + exprcat::kSurprisedHoldMs - 1));
  TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, mgr.active());
  TEST_ASSERT_TRUE(mgr.tick(1000 + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  TEST_ASSERT_FALSE(mgr.tick(1000 + exprcat::kSurprisedHoldMs + 1));
}

void test_exprmgr_play_once_retrigger_extends_hold() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 0));
  TEST_ASSERT_FALSE(mgr.tick(exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_FALSE(mgr.tick(2 * exprcat::kSurprisedHoldMs - 101));
  TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, mgr.active());
  TEST_ASSERT_TRUE(mgr.tick(2 * exprcat::kSurprisedHoldMs - 100));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
}

void test_exprmgr_hold_wraps_and_persists() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kSurprised, 0xFFFFFF00u));
  TEST_ASSERT_FALSE(mgr.tick(0xFFFFFF00u + 100));
  TEST_ASSERT_TRUE(mgr.tick(0xFFFFFF00u + exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, mgr.active());
  TEST_ASSERT_TRUE(mgr.request(expression::kSad, 5000));
  TEST_ASSERT_FALSE(mgr.tick(5000 + 100000));
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, mgr.active());
}

void test_exprmgr_unknown_ids_fail_closed() {
  expression::Manager mgr;
  TEST_ASSERT_TRUE(mgr.request(expression::kAlert, 0));
  const uint8_t bad[] = {5, 12, 99, 0xFF};
  for (size_t i = 0; i < sizeof(bad); ++i) {
    TEST_ASSERT_FALSE(mgr.request(bad[i], 100));
    TEST_ASSERT_EQUAL_UINT8(expression::kAlert, mgr.active());
  }
}

void test_exprtask_request_stages_dispatch_id() {
  expression::Task task;
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_TRUE(task.request(expression::kSad, 0));
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, task.staged());
  task.markDispatched();
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, task.active());
}

void test_exprtask_gated_request_keeps_latch() {
  expression::Task task;
  TEST_ASSERT_TRUE(task.request(expression::kAlert, 0));
  TEST_ASSERT_FALSE(task.request(expression::kHappy, 100));
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.staged());
  TEST_ASSERT_FALSE(task.request(99, 200));
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.staged());
}

void test_exprtask_tick_yield_stages_neutral() {
  expression::Task task;
  TEST_ASSERT_TRUE(task.request(expression::kSurprised, 0));
  task.markDispatched();
  TEST_ASSERT_FALSE(task.tick(exprcat::kSurprisedHoldMs - 1));
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_TRUE(task.tick(exprcat::kSurprisedHoldMs));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, task.active());
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, task.staged());
}

void test_exprtask_placeholders_stay_none() {
  static const uint8_t kV1[] = {0, 1, 2, 3, 4, 6, 7};
  for (size_t i = 0; i < sizeof(kV1); ++i) {
    const exprcat::Entry* e = exprcat::lookup(kV1[i]);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kSoundNone, e->sound);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kMotionNone, e->motion);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kNeckNone, e->neckPose);
    TEST_ASSERT_EQUAL_UINT8(exprcat::kArmNone, e->armPose);
  }
}

namespace {
// Host seam for the dispatch pump: mirrors LinkServer::sendUnicast.
struct FakeExprLink {
  bool ok = true;
  unsigned calls = 0;
  uint8_t lastType = 0;
  uint8_t lastByte = 0xFF;
  uint16_t lastLen = 0;
  bool sendUnicast(uint8_t type, const uint8_t* payload, uint16_t len, uint16_t& seqOut) {
    ++calls;
    if (!ok) {
      return false;
    }
    lastType = type;
    lastLen = len;
    lastByte = (payload != nullptr && len == 1) ? payload[0] : 0xFF;
    seqOut = 7;
    return true;
  }
};
}  // namespace

void test_exprtask_pump_sends_staged_once() {
  expression::Task task;
  FakeExprLink link;
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(0, link.calls);
  TEST_ASSERT_TRUE(task.request(expression::kSad, 0));
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(1, link.calls);
  TEST_ASSERT_EQUAL_UINT8(0x08, link.lastType);
  TEST_ASSERT_EQUAL_UINT16(1, link.lastLen);
  TEST_ASSERT_EQUAL_UINT8(expression::kSad, link.lastByte);
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(1, link.calls);
}

void test_exprtask_pump_refused_send_keeps_latch() {
  expression::Task task;
  FakeExprLink link;
  link.ok = false;
  TEST_ASSERT_TRUE(task.request(expression::kAlert, 0));
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT(1, link.calls);
  TEST_ASSERT_TRUE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, task.staged());
  link.ok = true;
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_FALSE(task.hasStaged());
  TEST_ASSERT_EQUAL_UINT8(expression::kAlert, link.lastByte);
}

void test_exprtask_pump_tick_yield_sends_neutral() {
  expression::Task task;
  FakeExprLink link;
  TEST_ASSERT_TRUE(task.request(expression::kSurprised, 0));
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT8(expression::kSurprised, link.lastByte);
  TEST_ASSERT_TRUE(task.tick(exprcat::kSurprisedHoldMs));
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_EQUAL_UINT8(expression::kBoot, link.lastByte);
}

namespace {
// Host seams for the bundle conductor: mirror the real sink APIs
// (AudioTask::cancelAll/beep, MotionController::stop/startWiggle) and record
// every call. nBeepsAtCancel/wigglesAtStop pin the cancel-before-start order.
struct FakeExprAudio {
  unsigned cancelCalls = 0;
  struct Beep {
    uint16_t freqHz;
    uint16_t durMs;
    uint16_t gapMs;
  };
  Beep beeps[16] = {};
  unsigned nBeeps = 0;
  unsigned nBeepsAtCancel = 0;
  void cancelAll() {
    ++cancelCalls;
    nBeepsAtCancel = nBeeps;
  }
  bool beep(uint16_t freqHz, uint16_t durMs, uint16_t gapMs = 0) {
    if (nBeeps < 16) {
      beeps[nBeeps++] = {freqHz, durMs, gapMs};
    }
    return true;
  }
};

struct FakeExprMotion {
  unsigned stops = 0;
  unsigned wiggles = 0;
  unsigned wigglesAtStop = 0;
  uint32_t durMs = 0;
  float rateHz = 0.0f;
  float amplitude = 0.0f;
  void stop() {
    ++stops;
    wigglesAtStop = wiggles;
  }
  void startWiggle(uint32_t durationMs, float rateHzIn = 2.0f, float amplitudeIn = 0.6f) {
    ++wiggles;
    durMs = durationMs;
    rateHz = rateHzIn;
    amplitude = amplitudeIn;
  }
};

void assertBeepsMatchCue(const FakeExprAudio& audio, unsigned base, const exprcue::Cue& cue) {
  TEST_ASSERT_EQUAL_UINT(cue.len, audio.nBeeps - base);
  for (uint8_t i = 0; i < cue.len; ++i) {
    TEST_ASSERT_EQUAL_UINT16(cue.notes[i].freqHz, audio.beeps[base + i].freqHz);
    TEST_ASSERT_EQUAL_UINT16(cue.notes[i].durMs, audio.beeps[base + i].durMs);
    TEST_ASSERT_EQUAL_UINT16(cue.notes[i].gapMs, audio.beeps[base + i].gapMs);
  }
}

void assertWiggleMatchesPrim(const FakeExprMotion& motion, const motionprim::Primitive& prim) {
  TEST_ASSERT_EQUAL_UINT32(prim.durationMs, motion.durMs);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, prim.rateHz, motion.rateHz);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, prim.amplitude, motion.amplitude);
}

}  // namespace

void test_exprcue_v2_cue_shapes() {
  const exprcue::Cue angry = exprcue::cueFor(exprcat::kSoundAngry);
  TEST_ASSERT_EQUAL_UINT8(2, angry.len);
  TEST_ASSERT_EQUAL_UINT16(130, angry.notes[0].freqHz);
  TEST_ASSERT_EQUAL_UINT16(98, angry.notes[1].freqHz);
  const exprcue::Cue scared = exprcue::cueFor(exprcat::kSoundScared);
  TEST_ASSERT_EQUAL_UINT8(3, scared.len);
  const exprcue::Cue excited = exprcue::cueFor(exprcat::kSoundExcited);
  TEST_ASSERT_EQUAL_UINT8(4, excited.len);
  for (uint8_t i = 1; i < excited.len; ++i) {
    TEST_ASSERT_TRUE(excited.notes[i].freqHz > excited.notes[i - 1].freqHz);
  }
  const exprcue::Cue confused = exprcue::cueFor(exprcat::kSoundConfused);
  TEST_ASSERT_EQUAL_UINT8(2, confused.len);
  TEST_ASSERT_EQUAL_UINT16(392, confused.notes[0].freqHz);
  TEST_ASSERT_EQUAL_UINT16(311, confused.notes[1].freqHz);
  TEST_ASSERT_TRUE(angry.len <= 4 && scared.len <= 4 && excited.len <= 4 && confused.len <= 4);
}

void test_exprcue_none_and_unknown_silent() {
  TEST_ASSERT_EQUAL_UINT8(0, exprcue::cueFor(exprcat::kSoundNone).len);
  TEST_ASSERT_NULL(exprcue::cueFor(exprcat::kSoundNone).notes);
  TEST_ASSERT_EQUAL_UINT8(0, exprcue::cueFor(0xFE).len);
  static const uint8_t kV1[] = {0, 1, 2, 3, 4, 6, 7};
  for (size_t i = 0; i < sizeof(kV1); ++i) {
    const exprcat::Entry* e = exprcat::lookup(kV1[i]);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0, exprcue::cueFor(e->sound).len);
  }
}

void test_motionprim_v2_params() {
  const motionprim::Primitive shake = motionprim::forId(exprcat::kMotionShake);
  TEST_ASSERT_FALSE(shake.still);
  TEST_ASSERT_TRUE(shake.rateHz > 2.0f);
  TEST_ASSERT_TRUE(shake.durationMs < 1200);
  const motionprim::Primitive cower = motionprim::forId(exprcat::kMotionCower);
  TEST_ASSERT_TRUE(cower.still);
  const motionprim::Primitive bounce = motionprim::forId(exprcat::kMotionBounce);
  TEST_ASSERT_FALSE(bounce.still);
  TEST_ASSERT_EQUAL_UINT32(1200, bounce.durationMs);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, bounce.rateHz);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, bounce.amplitude);
  const motionprim::Primitive tilt = motionprim::forId(exprcat::kMotionTilt);
  TEST_ASSERT_FALSE(tilt.still);
  TEST_ASSERT_TRUE(tilt.rateHz < 2.0f);
}

void test_motionprim_none_still() {
  TEST_ASSERT_TRUE(motionprim::forId(exprcat::kMotionNone).still);
  TEST_ASSERT_TRUE(motionprim::forId(0xFE).still);
}

void test_exprtask_conduct_fires_each_v2_bundle_once() {
  const uint8_t ids[] = {exprcat::kAngry, exprcat::kScared, exprcat::kExcited, exprcat::kConfused};
  for (size_t i = 0; i < sizeof(ids); ++i) {
    expression::Task task;
    FakeExprAudio audio;
    FakeExprMotion motion;
    TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
    TEST_ASSERT_TRUE(task.request(ids[i], 0));
    TEST_ASSERT_TRUE(task.hasBundle());
    TEST_ASSERT_EQUAL_UINT8(ids[i], task.bundle());
    TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
    TEST_ASSERT_EQUAL_UINT(1, audio.cancelCalls);
    TEST_ASSERT_EQUAL_UINT(1, motion.stops);
    const exprcat::Entry* e = exprcat::lookup(ids[i]);
    TEST_ASSERT_NOT_NULL(e);
    assertBeepsMatchCue(audio, 0, exprcue::cueFor(e->sound));
    const motionprim::Primitive prim = motionprim::forId(e->motion);
    if (prim.still) {
      TEST_ASSERT_EQUAL_UINT(0, motion.wiggles);
    } else {
      TEST_ASSERT_EQUAL_UINT(1, motion.wiggles);
      assertWiggleMatchesPrim(motion, prim);
    }
    TEST_ASSERT_FALSE(task.hasBundle());
    TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
    TEST_ASSERT_EQUAL_UINT(1, audio.cancelCalls);
  }
}

void test_exprtask_conduct_none_rows_silent_still() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  TEST_ASSERT_TRUE(task.request(expression::kHappy, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(1, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(1, motion.stops);
  TEST_ASSERT_EQUAL_UINT(0, audio.nBeeps);
  TEST_ASSERT_EQUAL_UINT(0, motion.wiggles);
  TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
}

void test_exprtask_conduct_preemption_cancels_first() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  TEST_ASSERT_TRUE(task.request(expression::kHappy, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_TRUE(task.request(exprcat::kAngry, 10));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(2, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(2, motion.stops);
  assertBeepsMatchCue(audio, 0, exprcue::cueFor(exprcat::kSoundAngry));
  TEST_ASSERT_EQUAL_UINT(1, motion.wiggles);
  assertWiggleMatchesPrim(motion, motionprim::forId(exprcat::kMotionShake));
  TEST_ASSERT_TRUE(task.request(exprcat::kScared, 20));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(3, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(2, audio.nBeepsAtCancel);
  TEST_ASSERT_EQUAL_UINT(3, motion.stops);
  TEST_ASSERT_EQUAL_UINT(1, motion.wigglesAtStop);
  assertBeepsMatchCue(audio, 2, exprcue::cueFor(exprcat::kSoundScared));
  TEST_ASSERT_EQUAL_UINT(1, motion.wiggles);
}

void test_exprtask_conduct_neutral_release_cancels() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  TEST_ASSERT_TRUE(task.request(exprcat::kAngry, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_TRUE(task.request(exprcat::kBoot, 10));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(2, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(2, motion.stops);
  TEST_ASSERT_EQUAL_UINT(2, audio.nBeeps);
  TEST_ASSERT_EQUAL_UINT(1, motion.wiggles);
  TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
}

void test_exprtask_conduct_playonce_yield_cancels() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  TEST_ASSERT_TRUE(task.request(expression::kSurprised, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(0, audio.nBeeps);
  TEST_ASSERT_EQUAL_UINT(0, motion.wiggles);
  TEST_ASSERT_TRUE(task.tick(exprcat::kSurprisedHoldMs));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(2, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(2, motion.stops);
  TEST_ASSERT_EQUAL_UINT(0, audio.nBeeps);
  TEST_ASSERT_EQUAL_UINT(0, motion.wiggles);
}

void test_exprtask_conduct_retrigger_restarts_bundle() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  TEST_ASSERT_TRUE(task.request(exprcat::kScared, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(3, audio.nBeeps);
  assertBeepsMatchCue(audio, 0, exprcue::cueFor(exprcat::kSoundScared));
  TEST_ASSERT_TRUE(task.request(exprcat::kScared, 10));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(2, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(2, motion.stops);
  TEST_ASSERT_EQUAL_UINT(6, audio.nBeeps);
  assertBeepsMatchCue(audio, 3, exprcue::cueFor(exprcat::kSoundScared));
  TEST_ASSERT_EQUAL_UINT(0, motion.wiggles);
}

void test_exprtask_conduct_rejected_requests_silent() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  TEST_ASSERT_TRUE(task.request(expression::kSurprised, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_FALSE(task.request(expression::kHappy, 100));
  TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
  TEST_ASSERT_FALSE(task.request(99, 200));
  TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(1, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(1, motion.stops);
  TEST_ASSERT_EQUAL_UINT(0, audio.nBeeps);
  TEST_ASSERT_EQUAL_UINT(0, motion.wiggles);
}

void test_exprtask_conduct_link_retry_no_replay() {
  expression::Task task;
  FakeExprAudio audio;
  FakeExprMotion motion;
  FakeExprLink link;
  TEST_ASSERT_TRUE(task.request(exprcat::kAngry, 0));
  TEST_ASSERT_TRUE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(2, audio.nBeeps);
  link.ok = false;
  TEST_ASSERT_FALSE(task.publishStaged(link, 0x08));
  TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(1, audio.cancelCalls);
  TEST_ASSERT_EQUAL_UINT(2, audio.nBeeps);
  TEST_ASSERT_EQUAL_UINT(1, motion.wiggles);
  link.ok = true;
  TEST_ASSERT_TRUE(task.publishStaged(link, 0x08));
  TEST_ASSERT_FALSE(task.conductBundle(audio, motion));
  TEST_ASSERT_EQUAL_UINT(1, audio.cancelCalls);
}
