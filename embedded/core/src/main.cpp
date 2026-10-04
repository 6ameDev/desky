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
#include "hal/vl53l0x_driver.h"
#include "middleware/link_manager.h"
#include "middleware/sensor_task.h"
#include "middleware/udp_server.h"
#include "services/config_store.h"
#include "services/event_bus.h"

namespace {
MotorDriver g_motorDriver(MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3, MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
MotionController g_motion;
Coordinator g_coordinator;
SensorTask g_sensor;
UdpServer g_udp;
// UART link to head (v2 carry-over from poc-comm-link): RX-only Serial1
// (RX=18, TX tristated until the first valid Head frame protects CAM GPIO12
// strapping), 16KB RX ring, 64KB reassembly slot. Background service —
// polled every loop tick alongside the brain tasks.
LinkManager g_link;
I2CManager g_i2c;
Mpu6500Driver g_mpu(g_i2c);
Vl53l0xDriver g_tof(g_i2c);
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
  const bool coordOk = g_coordinator.begin(&g_motion);
  DESKY_ASSERT(coordOk);
  const bool sensorOk = g_sensor.begin(&g_mpu, &g_tof);
  DESKY_ASSERT(sensorOk);
  const bool udpOk = g_udp.begin(&g_coordinator, &g_tof, &g_mpu);
  DESKY_ASSERT(udpOk);
  g_link.begin();  // RX-only Serial1 + INTERNAL buffers; never drives CAM GPIO12 until head frames validate.
  LOG_I("BOOT", "link rx=%d tx=%d(tristated until first head frame) ring=16K slot=64K", MCU_LINK_UART_RX,
        MCU_LINK_UART_TX);
  LOG_I("BOOT", "sensors ready tof=%dmm mpu=0x%02X", g_tof.distanceMm(), g_mpu.address());
  LOG_I("BOOT", "motor L0+L1 ready pins=%d,%d,%d,%d fault=%d", MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3,
        MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
  LOG_I("BOOT", "desky v2 boot | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
  LOG_I("BOOT", "cpu=%dMHz flash_chip=%dB", ESP.getCpuFreqMHz(), ESP.getFlashChipSize());
}

void loop() {
  FaultManager::watchdogFeed();
  g_link.poll();  // Link RX pump (bounded 64-pass drain) + USB CLI; needs ms-scale ticks.
  static uint32_t s_lastAliveMs = 0;
  const uint32_t now = millis();
  if (now - s_lastAliveMs >= 2000) {  // Wrap-safe elapsed gate (was blocking delay).
    s_lastAliveMs = now;
    LOG_I("MAIN", "desky v2 skeleton alive");
    Diagnostics::logWatermarks();
  }
  delay(1);
}
