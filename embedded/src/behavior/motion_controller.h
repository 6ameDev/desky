#pragma once
// desky v2 L1 open-loop velocity servo — header-only, 100Hz FreeRTOS task.
//
// Units (judgment call): NORMALIZED inputs -1.0..1.0 for both v (forward)
// and omega (turn; +omega == right). Physical units (m/s, rad/s) need
// wheelbase/track calibration that does not exist yet, so the L1 API stays
// unitless and the mapping below documents the single place where physical
// scaling will slot in. Duty output is int -255..255 into MotorDriver,
// which owns the MIN_PWM deadband.
//
// Mailbox: latest-wins struct guarded by a FreeRTOS mutex (matches the
// codebase's Logger/EventBus mutex style). Motion idles at stop: the task
// boots with a zero command and applies it every tick, so a silent
// coordinator means stopped wheels, not last-command drive.
//
// Loop: vTaskDelayUntil 10ms @ Core 1, stack 4096 (V1-proven), feeds the
// task watchdog every tick (FaultManager per-task subscription, same as
// loop()). FUTURE PID INJECTION POINT: close heading here — read fused
// yaw in loop(), run the heading PID between mailbox snapshot and
// mapToDuty(), add the correction to omega. No IMU yet, so this ships
// open-loop (see v2.md §F: Core 1 Task A 100Hz motor slot).
//
// Async maneuver (pattern proof): startWiggle() arms a timed open-loop
// wiggle evaluated by the 100Hz task — never blocks the caller. On expiry
// the task publishes EVENT_BEHAVIOR_DONE (payload=BEHAVIOR_HAPPY_WIGGLE)
// exactly once and returns to mailbox control. No STARTED event is
// published (DONE-only keeps the spike minimal; the commander already
// knows when it armed the maneuver). Cancellation via setVelocity/stop/
// brake produces NO DONE: cancellation is itself an event the commander
// caused, so it needs no completion signal.

#include <Arduino.h>

#include <cmath>
#include <cstdint>

#include "config.h"
#include "hal/motor_driver.h"
#include "services/diagnostics.h"
#include "services/event_bus.h"
#include "services/fault_manager.h"
#include "services/logger.h"
#include "system_context.h"

class MotionController {
 public:
  static constexpr uint32_t kLoopHz = CFG_MOTOR_LOOP_HZ;
  static constexpr uint32_t kPeriodMs = 1000 / CFG_MOTOR_LOOP_HZ;
  static constexpr BaseType_t kCore = 1;
  static constexpr uint32_t kStackWords = 4096;
  // Above loopTask (prio 1, V1 used 1 for its combined HW task) so 100Hz
  // timing holds once sensor/coordinator tasks land; still far below
  // WiFi/IPC internals. Revisit when the §F task map fills in.
  static constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + 5;
  static constexpr int kMaxDuty = 255;

  struct Command {
    float v = 0.0f;
    float omega = 0.0f;
  };

  MotionController() : driver_(nullptr), mutex_(nullptr), task_(nullptr) {}

  // Creates the mailbox mutex and pins the servo task to Core 1. The task
  // starts from stop and holds it until setVelocity() is called.
  bool begin(MotorDriver* driver) {
    DESKY_ASSERT(driver != nullptr);
    if (driver == nullptr) {
      return false;
    }
    driver_ = driver;
    mutex_ = xSemaphoreCreateMutex();
    DESKY_ASSERT(mutex_ != nullptr);
    if (mutex_ == nullptr) {
      return false;
    }
    const BaseType_t ok =
        xTaskCreatePinnedToCore(&MotionController::taskEntry, "motion", kStackWords, this, kPriority, &task_, kCore);
    DESKY_ASSERT(ok == pdPASS);
    return ok == pdPASS;
  }

  void setVelocity(float v, float omega) {
    Command cmd{clampUnit(v), clampUnit(omega)};
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cmd_ = cmd;
    // Preemption: a fresh command cancels any active maneuver immediately.
    // No DONE is published on cancel (see header comment).
    maneuver_ = Maneuver::kIdle;
    xSemaphoreGive(mutex_);
  }

  void stop() { setVelocity(0.0f, 0.0f); }

  // E-stop latch: cancels any maneuver, zeroes the mailbox, and forces an
  // active short-stop now. Following ticks hold (0,0) via setDuty (coast);
  // re-issue brake() to re-assert the short while parked.
  void brake() {
    if (mutex_ == nullptr) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    cmd_ = Command{};
    maneuver_ = Maneuver::kIdle;
    xSemaphoreGive(mutex_);
    if (driver_ != nullptr) {
      driver_->brake();
    }
  }

  // Non-blocking timed wiggle: sinusoidal differential drive (v=0,
  // omega=amplitude*sin(2*pi*rateHz*t)) through mapToDuty() + the driver's
  // MIN_PWM floor. Latest-wins: re-arming supersedes without a DONE for the
  // superseded run. Zero duration is a no-op. No heap/String; task-safe.
  void startWiggle(uint32_t durationMs, float rateHz = 2.0f, float amplitude = 0.6f) {
    if (mutex_ == nullptr || durationMs == 0 || rateHz <= 0.0f) {
      return;
    }
    const uint32_t startMs = millis();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    wiggleStartMs_ = startMs;
    wiggleDurationMs_ = durationMs;
    wiggleRateHz_ = rateHz;
    wiggleAmplitude_ = clampUnit(amplitude);
    if (wiggleAmplitude_ < 0.0f) {
      wiggleAmplitude_ = -wiggleAmplitude_;
    }
    maneuver_ = Maneuver::kWiggle;
    xSemaphoreGive(mutex_);
  }

  // Pure differential-drive mix, unit-testable with no hardware:
  // left = v - omega, right = v + omega, clamped, scaled to ±255.
  // Deadband/MIN_PWM lives in MotorDriver::setDuty, not here.
  static void mapToDuty(float v, float omega, int& left, int& right) {
    float l = clampUnit(v) - clampUnit(omega);
    float r = clampUnit(v) + clampUnit(omega);
    l = clampUnit(l);
    r = clampUnit(r);
    left = static_cast<int>(roundf(l * kMaxDuty));
    right = static_cast<int>(roundf(r * kMaxDuty));
  }

 private:
  static float clampUnit(float x) {
    if (x > 1.0f) {
      return 1.0f;
    }
    if (x < -1.0f) {
      return -1.0f;
    }
    return x;
  }

  static void taskEntry(void* arg) { static_cast<MotionController*>(arg)->loop(); }

  void loop() {
    DESKY_ASSERT(driver_ != nullptr);
    esp_task_wdt_add(nullptr);
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(kPeriodMs);
    uint32_t ticks = 0;
    for (;;) {
      vTaskDelayUntil(&lastWake, period);
      const uint32_t nowMs = millis();
      xSemaphoreTake(mutex_, portMAX_DELAY);
      Command cmd = cmd_;
      bool expired = false;
      if (maneuver_ == Maneuver::kWiggle) {
        // Unsigned subtraction: wrap-safe across the ~49-day millis() rollover.
        const uint32_t elapsedMs = nowMs - wiggleStartMs_;
        if (elapsedMs >= wiggleDurationMs_) {
          // DONE-once: transition to IDLE under the mutex before publishing,
          // so the expiry branch can never re-fire even under preemption races.
          maneuver_ = Maneuver::kIdle;
          expired = true;
        } else {
          const float tS = static_cast<float>(elapsedMs) / 1000.0f;
          cmd.v = 0.0f;
          cmd.omega = wiggleAmplitude_ * sinf(kTwoPi * wiggleRateHz_ * tS);
        }
      }
      xSemaphoreGive(mutex_);
      if (expired) {
        // Task context: the logging publish variant is allowed here.
        EventBus::publish(EVENT_BEHAVIOR_DONE, static_cast<uint32_t>(BEHAVIOR_HAPPY_WIGGLE));
      }
      // FUTURE PID INJECTION POINT: fused yaw -> heading PID corrects
      // cmd.omega here (open-loop until the IMU lands).
      int left = 0;
      int right = 0;
      mapToDuty(cmd.v, cmd.omega, left, right);
      driver_->setDuty(left, right);
      FaultManager::watchdogFeed();
      ++ticks;
      if (ticks % (kLoopHz * 5) == 0) {
        Diagnostics::logWatermarks("MOTION");
      }
    }
  }

  enum class Maneuver : uint8_t { kIdle, kWiggle };

  static constexpr float kTwoPi = 6.283185307179586f;

  MotorDriver* driver_;
  SemaphoreHandle_t mutex_;
  TaskHandle_t task_;
  Command cmd_;
  Maneuver maneuver_ = Maneuver::kIdle;
  uint32_t wiggleStartMs_ = 0;
  uint32_t wiggleDurationMs_ = 0;
  float wiggleRateHz_ = 2.0f;
  float wiggleAmplitude_ = 0.6f;
};
