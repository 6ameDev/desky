#include <Arduino.h>

#include "behavior/coordinator.h"
#include "behavior/motion_controller.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/i2c_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/motor_driver.h"
#include "hal/mpu6500_driver.h"
#include "hal/tcrt5000_driver.h"
#include "hal/vl53l0x_driver.h"
#include "middleware/sensor_task.h"
#include "middleware/udp_server.h"
#include "services/audio_manager.h"
#include "services/config_store.h"
#include "services/event_bus.h"

// Bench-mode WiFi kill (desky-bench env sets -D DESKY_NO_WIFI=1): default
// build has WiFi on. Same fallback pattern as DESKY_WIFI_STA in udp_server.h.
#ifndef DESKY_NO_WIFI
#define DESKY_NO_WIFI 0
#endif

namespace {
MotorDriver g_motorDriver(MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3, MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
MotionController g_motion;
Coordinator g_coordinator;
SensorTask g_sensor;
AudioManager g_audio;
UdpServer g_udp;
I2CManager g_i2c;
Mpu6500Driver g_mpu(g_i2c);
Vl53l0xDriver g_tof(g_i2c);
Tcrt5000Driver g_tcrtFwd(CFG_TCRT_FWD_PIN, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, CFG_TCRT_ASSERT_HIGH);
Tcrt5000Driver g_tcrtRev(CFG_TCRT_REV_PIN, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, CFG_TCRT_ASSERT_HIGH);
}  // namespace

void setup() {
  Serial.begin(115200);
  Logger::begin();
  EventBus::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  ConfigStore::begin();
  const bool motorOk = g_motorDriver.init();
  DESKY_ASSERT(motorOk);
  const bool motionOk = g_motion.begin(&g_motorDriver);
  DESKY_ASSERT(motionOk);
  const bool i2cOk = g_i2c.begin();
  DESKY_ASSERT(i2cOk);
  g_i2c.scanBus();
  const bool tofOk = g_tof.init();
  DESKY_ASSERT(tofOk);
  const bool mpuOk = g_mpu.init();
  DESKY_ASSERT(mpuOk);
  const bool tcrtFwdOk = g_tcrtFwd.init();
  DESKY_ASSERT(tcrtFwdOk);
  const bool tcrtRevOk = g_tcrtRev.init();
  DESKY_ASSERT(tcrtRevOk);
  const bool coordOk = g_coordinator.begin(&g_motion);
  DESKY_ASSERT(coordOk);
  const bool sensorOk = g_sensor.begin(&g_mpu, &g_tof, &g_tcrtFwd, &g_tcrtRev);
  DESKY_ASSERT(sensorOk);
  const bool audioOk = g_audio.begin();
  DESKY_ASSERT(audioOk);
#if DESKY_NO_WIFI
  // Bench mode (desky-bench env): WiFi stays off — GPIO13/14 are ADC2, which
  // conflicts with WiFi, so the TCRT rails need it off. The coordinator then
  // sees no UDP commands and sits in failsafe stop — safe by construction.
  LOG_I("BOOT", "wifi OFF bench mode (DESKY_NO_WIFI=1): TCRT rails live, coordinator failsafe-stopped");
#else
  const bool udpOk = g_udp.begin(&g_coordinator, &g_tof, &g_mpu);
  DESKY_ASSERT(udpOk);
#endif
  LOG_I("BOOT", "sensors ready tof=%dmm mpu=0x%02X", g_tof.distanceMm(), g_mpu.address());
  LOG_I("BOOT", "tcrt fwd pin=%d raw=%u gnd=%u rev pin=%d raw=%u gnd=%u", CFG_TCRT_FWD_PIN, g_tcrtFwd.rawCount(),
        g_tcrtFwd.groundPresent() ? 1u : 0u, CFG_TCRT_REV_PIN, g_tcrtRev.rawCount(),
        g_tcrtRev.groundPresent() ? 1u : 0u);
  LOG_I("BOOT", "motor L0+L1 ready pins=%d,%d,%d,%d fault=%d", MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3,
        MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
  LOG_I("BOOT", "desky v2 boot | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
  LOG_I("BOOT", "cpu=%dMHz flash_chip=%dB", ESP.getCpuFreqMHz(), ESP.getFlashChipSize());
  // Speaker self-test in every env (incl. bench): the jingle only queues
  // here — the audio task synths it — so a silent bench still boots clean.
  const bool jingleOk = g_audio.bootJingle();
  DESKY_ASSERT(jingleOk);
}

void loop() {
  FaultManager::watchdogFeed();
  LOG_I("MAIN", "desky v2 skeleton alive");
  Diagnostics::logWatermarks();
  delay(2000);
}
