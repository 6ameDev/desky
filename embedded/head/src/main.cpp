#include <Arduino.h>

#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/i2c_manager.h"
#include "common/log_filter.h"
#include "common/logger.h"
#include "config.h"
#include "hal/camera_driver.h"
#include "hal/ssd1306_display.h"
#include "head_context.h"
#include "middleware/face_renderer.h"
#include "middleware/link_stub.h"
#include "services/cli_manager.h"
#include "services/event_bus.h"
#include "services/power_manager.h"

// desky-head (face-unit: OV3660 eyes + face OLED + link stub, PARKED).
// Setup order (core convention): logger -> fault -> config -> banner —
// nothing that can fail runs before logging exists.
//
// Hardware map (see include/mcu/esp32cam_ov3660.h):
// - OLED SDA=GPIO14/SCL=GPIO15 (address scanned 0x3C/0x3D, never assumed;
//   absent on every MB USB run by design — "not found" never blocks boot).
// - Wired link PARKED (no copper; wireless TBD; stub retained for switch-back).
// - GPIO2 + GPIO4 reserved.
// - Star 5V power from the common rail.

// Fallbacks keep main compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_CLI_BAUD
#define CFG_CLI_BAUD 115200
#endif
#ifndef CFG_CAMERA_POLL_MS
#define CFG_CAMERA_POLL_MS 500
#endif
#ifndef HEAD_BOOT_LEAN
#define HEAD_BOOT_LEAN 0
#endif

namespace {
I2CManager g_i2c;
CameraDriver g_cam;
Ssd1306Display g_panel(g_i2c);
FaceRenderer g_display(g_panel);
PowerManager g_power;
LinkStub g_link;
CliManager g_cli;
EventBus::Subscription g_sub;
uint32_t g_lastCamMs = 0;
uint32_t g_lastAliveMs = 0;
}  // namespace

void setup() {
  Serial.begin(CFG_CLI_BAUD);
  Logger::begin();
  EventBus::begin();
  FaultManager::watchdogInit();
  FaultManager::registerAllocFailureHook();
  const bool i2cOk = g_i2c.begin();
  DESKY_ASSERT(i2cOk);
  g_i2c.scanBus();  // On-MB this shows no OLED — expected, never a fault.
  // Known esp32-camera quirk: esp_camera_deinit() leaves the system-wide
  // GPIO ISR service installed, so every later esp_camera_init() logs
  // "GPIO isr service already installed" at ERROR and succeeds anyway.
  // Suppressed by exact phrase (all other gpio errors still print); revisit
  // if off/on cycling ever shows resource drift in status (frames/drops
  // across ~10 cycles).
  LogFilter::addRule("GPIO isr service already installed");
  LogFilter::begin();
  LOG_I("LOG", "filter rules=%u", LogFilter::ruleCount());
  const bool camOk = g_cam.init();
  DESKY_ASSERT(camOk);  // Camera works on-MB (ribbon only), unlike the OLED.
  const bool oledOk = g_display.init();
  (void)oledOk;  // NEVER asserted: absent OLED is the normal on-MB state.
  g_power.begin(&g_cam, &g_display);
  g_link.begin(&g_power);  // PARKED stub (retained for switch-back).
  g_cli.begin(&g_power);
  g_sub = EventBus::subscribe();  // Settles before any publish (loop only).
  g_lastCamMs = millis();
  g_lastAliveMs = millis();
  LOG_I("BOOT", "desky-head alive | MCU=%s cores=%d flash=%dMB psram=%d", MCU_NAME, MCU_NUM_CORES, MCU_FLASH_SIZE_MB,
        MCU_HAS_PSRAM);
  LOG_I("BOOT", "pins oled=%d,%d link=%d,%d lean=%d", MCU_OLED_SDA, MCU_OLED_SCL, MCU_LINK_UART_TX, MCU_LINK_UART_RX,
        HEAD_BOOT_LEAN);
  LOG_I("BOOT", "reset=%s sdk=%s rev=%d", FaultManager::resetReasonStr(), ESP.getSdkVersion(), ESP.getChipRevision());
}

void loop() {
  FaultManager::watchdogFeed();
  g_cli.poll();   // USB door (human).
  g_link.poll();  // UART door (S3 verbs into the same command table).
  g_link.heartbeatTick();
  const uint32_t now = millis();
  if (now - g_lastCamMs >= CFG_CAMERA_POLL_MS) {  // Wrap-safe elapsed gate.
    g_lastCamMs = now;
    g_cam.update();  // Grab-and-drop still (no-op while camera off).
  }
  HeadEvent ev;
  while (g_sub.receive(ev, 0)) {  // Non-blocking drain; logs at DEBUG.
    LOG_D("BUS", "ev %s payload=%lu", headEventName(ev.type), static_cast<unsigned long>(ev.payload));
  }
  if (now - g_lastAliveMs >= 5000) {
    g_lastAliveMs = now;
    LOG_I("HEAD", "alive cam=%s oled=%s face=%s", camStateName(g_power.camState()), oledStateName(g_power.oledState()),
          faceName(g_power.faceId()));
    Diagnostics::logWatermarks("HEAD");
  }
}
