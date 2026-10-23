// Host unit tests for the system mode policy (no hardware).
// Run: pio test -e native-test
//
// Covers sysmode::apply() (every transition + every exit), the failsafe
// overlay in each mode, the drive-gate default, the display mapping, and the
// pairing window-open signal the coordinator feeds the policy with. The
// Coordinator task drain (event feed + gate consult) is firmware-only
// (#ifdef ARDUINO) and exercised on hardware via synthetic events.
//
// Runner lives in test_udp_codec.cpp (single main for the native-test
// binary): the mode tests are declared extern there.

#include <unity.h>

#include "../src/abilities/pairing/pairing_manager.h"
#include "../src/behavior/mode_manager.h"

namespace {

const uint8_t kCode[pairing::kCodeLen] = {'1', '2', '3', '4', '5', '6'};

sysmode::State inMode(sysmode::Mode m) {
  sysmode::State s;
  s.mode = m;
  return s;
}

}  // namespace

void test_mode_starts_normal() {
  const sysmode::State s;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(s.mode));
}

void test_mode_pair_window_open_enters_pairing() {
  const sysmode::State fromNormal = sysmode::apply(sysmode::State(), sysmode::Event::kPairWindowOpened);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kPairing), static_cast<int>(fromNormal.mode));
  const sysmode::State fromManual = sysmode::apply(inMode(sysmode::Mode::kManual), sysmode::Event::kPairWindowOpened);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kPairing), static_cast<int>(fromManual.mode));
  // Repeating the open while pairing is a no-op (level-fed every tick).
  const sysmode::State repeat = sysmode::apply(inMode(sysmode::Mode::kPairing), sysmode::Event::kPairWindowOpened);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kPairing), static_cast<int>(repeat.mode));
}

void test_mode_paired_exits_pairing_to_normal() {
  const sysmode::State out = sysmode::apply(inMode(sysmode::Mode::kPairing), sysmode::Event::kPaired);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(out.mode));
}

void test_mode_pair_window_closed_exits_pairing_to_normal() {
  const sysmode::State out = sysmode::apply(inMode(sysmode::Mode::kPairing), sysmode::Event::kPairWindowClosed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(out.mode));
}

void test_mode_ota_staged_and_started_enter_ota() {
  const sysmode::Mode from[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kPairing};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State staged = sysmode::apply(inMode(from[i]), sysmode::Event::kOtaStaged);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kOta), static_cast<int>(staged.mode));
    const sysmode::State started = sysmode::apply(inMode(from[i]), sysmode::Event::kOtaStarted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kOta), static_cast<int>(started.mode));
  }
}

void test_mode_ota_done_and_aborted_exit_ota_to_normal() {
  const sysmode::State done = sysmode::apply(inMode(sysmode::Mode::kOta), sysmode::Event::kOtaDone);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(done.mode));
  const sysmode::State aborted = sysmode::apply(inMode(sysmode::Mode::kOta), sysmode::Event::kOtaAborted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(aborted.mode));
}

void test_mode_manual_take_granted_enters_manual_from_normal() {
  const sysmode::State out = sysmode::apply(sysmode::State(), sysmode::Event::kManualTakeGranted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kManual), static_cast<int>(out.mode));
}

void test_mode_manual_release_granted_exits_manual_to_normal() {
  const sysmode::State out = sysmode::apply(inMode(sysmode::Mode::kManual), sysmode::Event::kManualReleaseGranted);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(out.mode));
}

void test_mode_command_stale_exits_manual_to_normal() {
  const sysmode::State out = sysmode::apply(inMode(sysmode::Mode::kManual), sysmode::Event::kCommandStale);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kNormal), static_cast<int>(out.mode));
}

void test_mode_paired_outside_pairing_is_noop() {
  const sysmode::Mode held[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kOta};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State out = sysmode::apply(inMode(held[i]), sysmode::Event::kPaired);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(out.mode));
  }
}

void test_mode_ota_done_outside_ota_is_noop() {
  const sysmode::Mode held[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kPairing};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State done = sysmode::apply(inMode(held[i]), sysmode::Event::kOtaDone);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(done.mode));
    const sysmode::State aborted = sysmode::apply(inMode(held[i]), sysmode::Event::kOtaAborted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(aborted.mode));
  }
}

void test_mode_window_closed_outside_pairing_is_noop() {
  const sysmode::Mode held[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kOta};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State out = sysmode::apply(inMode(held[i]), sysmode::Event::kPairWindowClosed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(out.mode));
  }
}

void test_mode_manual_take_guarded_outside_normal() {
  const sysmode::Mode held[] = {sysmode::Mode::kManual, sysmode::Mode::kPairing, sysmode::Mode::kOta};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State out = sysmode::apply(inMode(held[i]), sysmode::Event::kManualTakeGranted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(out.mode));
  }
}

void test_mode_manual_release_outside_manual_is_noop() {
  const sysmode::Mode held[] = {sysmode::Mode::kNormal, sysmode::Mode::kPairing, sysmode::Mode::kOta};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State out = sysmode::apply(inMode(held[i]), sysmode::Event::kManualReleaseGranted);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(out.mode));
  }
}

void test_mode_command_stale_outside_manual_never_changes_mode() {
  // The overlay inhibits motion without changing the mode in any other mode.
  const sysmode::Mode held[] = {sysmode::Mode::kNormal, sysmode::Mode::kPairing, sysmode::Mode::kOta};
  for (size_t i = 0; i < 3; ++i) {
    const sysmode::State out = sysmode::apply(inMode(held[i]), sysmode::Event::kCommandStale);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(held[i]), static_cast<int>(out.mode));
  }
}

void test_mode_pair_window_blocked_during_ota() {
  // OTA preempts everything: a window opened mid-update stays an update.
  const sysmode::State out = sysmode::apply(inMode(sysmode::Mode::kOta), sysmode::Event::kPairWindowOpened);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Mode::kOta), static_cast<int>(out.mode));
}

void test_mode_failsafe_inhibits_in_every_mode() {
  const sysmode::Mode modes[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kPairing,
                                 sysmode::Mode::kOta};
  for (size_t i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(sysmode::motionInhibited(modes[i], true));
  }
}

void test_mode_failsafe_clear_when_fresh_in_every_mode() {
  const sysmode::Mode modes[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kPairing,
                                 sysmode::Mode::kOta};
  for (size_t i = 0; i < 4; ++i) {
    TEST_ASSERT_FALSE(sysmode::motionInhibited(modes[i], false));
  }
}

void test_mode_drive_allowed_manual_only_when_paired() {
  // MANUAL-only gate: verified drive verifies/consumes in every mode, but
  // stick intent reaches motion solely while the MANUAL session is held.
  TEST_ASSERT_TRUE(sysmode::manualDriveAllowed(sysmode::Mode::kManual, true));
  TEST_ASSERT_FALSE(sysmode::manualDriveAllowed(sysmode::Mode::kNormal, true));
  TEST_ASSERT_FALSE(sysmode::manualDriveAllowed(sysmode::Mode::kPairing, true));
  TEST_ASSERT_FALSE(sysmode::manualDriveAllowed(sysmode::Mode::kOta, true));
}

void test_mode_drive_denied_when_unpaired_in_every_mode() {
  const sysmode::Mode modes[] = {sysmode::Mode::kNormal, sysmode::Mode::kManual, sysmode::Mode::kPairing,
                                 sysmode::Mode::kOta};
  for (size_t i = 0; i < 4; ++i) {
    TEST_ASSERT_FALSE(sysmode::manualDriveAllowed(modes[i], false));
  }
}

void test_mode_display_follows_mode() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Display::kFace),
                        static_cast<int>(sysmode::displayFor(sysmode::Mode::kNormal)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Display::kFace),
                        static_cast<int>(sysmode::displayFor(sysmode::Mode::kManual)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Display::kPairing),
                        static_cast<int>(sysmode::displayFor(sysmode::Mode::kPairing)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sysmode::Display::kOta),
                        static_cast<int>(sysmode::displayFor(sysmode::Mode::kOta)));
}

void test_mode_pair_window_signal_tracks_core() {
  pairing::Core core;
  TEST_ASSERT_FALSE(core.windowOpen(1000));
  TEST_ASSERT_TRUE(core.openWindow(1000, kCode));
  TEST_ASSERT_TRUE(core.windowOpen(1000));
  TEST_ASSERT_TRUE(core.windowOpen(1000 + pairing::kWindowMs - 1));
  TEST_ASSERT_FALSE(core.windowOpen(1000 + pairing::kWindowMs));
  // Burned by a good code: the window is consumed.
  TEST_ASSERT_EQUAL(static_cast<int>(pairing::Result::kOk), static_cast<int>(core.tryCode(kCode, 6, 2000)));
  TEST_ASSERT_FALSE(core.windowOpen(2000));
  // Reopen after a forget (the unpair path clears first) then clear: idle again.
  core.clearPair();
  TEST_ASSERT_TRUE(core.openWindow(3000, kCode));
  TEST_ASSERT_TRUE(core.windowOpen(3000));
  core.clearPair();
  TEST_ASSERT_FALSE(core.windowOpen(3000));
}
