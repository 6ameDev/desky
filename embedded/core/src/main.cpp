#include <Arduino.h>
#include <esp_system.h>

#include "abilities/audio/audio_task.h"
#include "abilities/expression/expression_task.h"
#include "abilities/ota/ota_task.h"
#include "abilities/pairing/pairing_task.h"
#include "abilities/sensors/sensors_task.h"
#include "abilities/telemetry/telemetry_task.h"
#include "behavior/motion_controller.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/i2c_manager.h"
#include "common/logger.h"
#include "config.h"
#include "hal/motor_driver.h"
#include "hal/mpu6500_driver.h"
#include "hal/pair_button.h"
#include "hal/tcrt5000_driver.h"
#include "hal/vl53l0x_driver.h"
#include "services/cli_manager.h"
#include "services/config_store.h"
#include "services/event_bus.h"
#include "transport/link_server.h"
#include "transport/udp_server.h"
// behavior/coordinator.h after transport/udp_server.h — its firmware section
// needs udpstatus first.
#include "behavior/coordinator.h"

// Bench-mode WiFi kill (desky-bench env sets -D DESKY_NO_WIFI=1): default
// build has WiFi on.
#ifndef DESKY_NO_WIFI
#define DESKY_NO_WIFI 0
#endif

namespace {
MotorDriver g_motorDriver(MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3, MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
MotionController g_motion;
Coordinator g_coordinator;
SensorTask g_sensor;
AudioTask g_audio;
UdpServer g_udp;
pairing::PairingTask g_pairing;
telemetry::TelemetryManager g_telem(UdpServer::kTelemetryMs, UdpServer::kStaleMs);
TelemetryTask g_telemetry;
LinkServer g_link;
expression::Task g_expression;  // Core-owned face ids, unicast to head via the pump below.
OtaStaging g_otaStage;          // Carrier head-image staging (POST /head.bin + GETs).
CoreCli g_cli;                  // Minimal USB door: `otahead <url> [version]` only.
// Binder adapter: the CLI parses + triggers through this sink; the ability
// object stays owned here in main.cpp.
struct CoreCliOta : ICliOtaSink {
  bool stageOtaUrl(const char* url) override { return g_otaStage.stageOtaUrl(url); }
  size_t stagedOtaLen() const override { return g_otaStage.stagedOtaLen(); }
};
CoreCliOta g_cliOta;
I2CManager g_i2c;
Mpu6500Driver g_mpu(g_i2c);
Vl53l0xDriver g_tof(g_i2c);
Tcrt5000Driver g_tcrtFwd(CFG_TCRT_FWD_PIN, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, CFG_TCRT_ASSERT_HIGH);
Tcrt5000Driver g_tcrtRev(CFG_TCRT_REV_PIN, CFG_TCRT_THRESHOLD, CFG_TCRT_HYSTERESIS, CFG_TCRT_ASSERT_HIGH);
PairButton g_pairButton(CFG_PAIR_BUTTON_PIN, CFG_PAIR_BUTTON_DEBOUNCE_MS);
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
  const bool pairBtnOk = g_pairButton.init();
  DESKY_ASSERT(pairBtnOk);
  const bool coordOk = g_coordinator.begin(&g_motion);
  DESKY_ASSERT(coordOk);
  const bool sensorOk = g_sensor.begin(&g_mpu, &g_tof, &g_tcrtFwd, &g_tcrtRev);
  DESKY_ASSERT(sensorOk);
  const bool audioOk = g_audio.begin();
  DESKY_ASSERT(audioOk);
#if DESKY_NO_WIFI
  // Bench mode: WiFi stays off for TCRT bring-up. The coordinator then sees
  // no UDP commands and sits in failsafe stop.
  LOG_I("BOOT", "wifi OFF bench mode (DESKY_NO_WIFI=1): TCRT rails live, coordinator failsafe-stopped");
#else
  // Transport owns dumb pipes only: the verified-drive fan-out is a
  // setup-registered hook (no bus include in transport), the peer/alive
  // binding + pair restore lands via attachPairing; the behavior pins
  // (pairing seam + telemetry RX-observation report) are wired directly on
  // the coordinator.
  g_udp.setCommandPublisher([](uint32_t payload) { EventBus::publish(EVENT_UDP_COMMAND_RECEIVED, payload); });
  const bool udpOk = g_udp.begin();
  DESKY_ASSERT(udpOk);
  g_udp.attachPairing(&g_pairing);
  g_coordinator.attachPairing(&g_pairing);
  g_coordinator.attachTelemetry(&g_telem);
  g_coordinator.attachExpression(&g_expression);
  const bool telemOk = g_telemetry.begin(&g_coordinator, &g_tof, &g_mpu, &g_telem, &g_udp);
  DESKY_ASSERT(telemOk);
  // Link endpoint (sockets only): never bricks driving — a bind or task
  // failure only warns and the boot continues headless.
  const bool linkOk = g_link.begin();
  if (!linkOk) {
    LOG_W("LINK", "link endpoint failed, continuing headless");
  }
  g_pairing.attachHeadLink(&g_link);
  // Carrier OTA staging: same never-brick rule — a staging failure only
  // warns, the `otahead` verb then fails closed at stage time.
  const bool otaStageOk = g_otaStage.begin();
  if (!otaStageOk) {
    LOG_W("OTA", "staging server failed, continuing without it");
  }
#endif
  // Minimal USB door (carrier `otahead` trigger): always live, even in bench
  // mode (the stage then fails closed).
  g_cli.begin(&g_cliOta);
  LOG_I("BOOT", "sensors ready tof=%dmm mpu=0x%02X", g_tof.distanceMm(), g_mpu.address());
  LOG_I("BOOT", "tcrt fwd pin=%d raw=%u gnd=%u rev pin=%d raw=%u gnd=%u", CFG_TCRT_FWD_PIN, g_tcrtFwd.rawCount(),
        g_tcrtFwd.groundPresent() ? 1u : 0u, CFG_TCRT_REV_PIN, g_tcrtRev.rawCount(),
        g_tcrtRev.groundPresent() ? 1u : 0u);
  LOG_I("BOOT", "motor L0+L1 ready pins=%d,%d,%d,%d fault=%d", MCU_MOTOR_IN1, MCU_MOTOR_IN2, MCU_MOTOR_IN3,
        MCU_MOTOR_IN4, MCU_MOTOR_FAULT);
  LOG_I("BOOT", "desky v2 boot | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
  // Random boot id joins app wall-clock markers to this boot's robot
  // millis() timeline. Logged once per reboot.
  LOG_I("BOOT", "boot id=%08lx", static_cast<unsigned long>(esp_random()));
  LOG_I("BOOT", "cpu=%dMHz flash_chip=%dB", ESP.getCpuFreqMHz(), ESP.getFlashChipSize());
  // Speaker self-test in every env (incl. bench): the jingle only queues
  // here — the audio task synths it.
  const bool jingleOk = g_audio.bootJingle();
  DESKY_ASSERT(jingleOk);
}

void loop() {
  FaultManager::watchdogFeed();
  // Minimal USB door: polled every loop; the stage waits silently, never
  // blocks driving.
  g_cli.poll();
  if (g_pairButton.poll()) {
#if DESKY_NO_WIFI
    LOG_I("PAIRBTN", "pair button pressed");
#else
    const bool opened = g_pairing.enterPairMode(g_udp, millis());
    LOG_I("PAIRBTN", "pair button pressed window=%u", opened ? 1u : 0u);
#endif
  }
#if !DESKY_NO_WIFI
  // Ability link pumps: the pairing window's PIN + valid-seconds and the
  // staged `otahead` URL each unicast once to the HB-pinned head. The staged
  // expression id rides EXPR the same way (refused sends retry next loop).
  // No head linked -> the pumps wait silently. Duplicate protection is the
  // link seq/ACK machinery.
  g_pairing.publishPairShow(g_link);
  g_pairing.pumpEvictionBurst(g_udp, millis());
  g_otaStage.pumpOtaLink(g_link);
  if (g_expression.publishStaged(g_link, linkcode::kTypeExpr)) {
    LOG_I("LINK", "expr sent id=%u", static_cast<unsigned>(g_expression.staged()));
  }
  // Telemetry consumer: snapshot + encode + publish at CFG_TELEMETRY_HZ
  // through the transport's dumb send (outer cadence inside poll —
  // non-blocking, silent off-cadence).
  g_telemetry.poll(millis());
#endif
#if !DESKY_NO_WIFI
  // The loop runs hot (~5ms) for the 15 Hz telemetry cadence; the heartbeat
  // + watermarks stay on the ~2 s cadence.
  static uint32_t lastWmMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastWmMs >= 2000) {
    lastWmMs = nowMs;
    LOG_D("MAIN", "desky v2 skeleton alive");
    Diagnostics::logWatermarks();
  }
  delay(5);
#else
  LOG_D("MAIN", "desky v2 skeleton alive");
  Diagnostics::logWatermarks();
  delay(2000);
#endif
}
