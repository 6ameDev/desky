#include <Arduino.h>

#include "abilities/face/face_task.h"
#include "common/diagnostics.h"
#include "common/fault_manager.h"
#include "common/i2c_manager.h"
#include "common/log_filter.h"
#include "common/logger.h"
#include "config.h"
#include "hal/camera_driver.h"
#include "hal/ssd1306_display.h"
#include "head_context.h"
#include "link/ota_manifest.h"
#include "middleware/link_stub.h"
#include "middleware/ota_client.h"
#include "middleware/wifi_link.h"
#include "services/cli_manager.h"
#include "services/event_bus.h"
#include "services/power_manager.h"
#include "transport/link_client.h"
#include "transport/link_stub.h"

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
LinkClient g_wifi;
OtaUpdater g_ota;  // Standing rule: never ship without the OTA client (see
                   // middleware/ota_client.h) — no USB door exists in the field.
CliManager g_cli;
EventBus::Subscription g_sub;
uint32_t g_lastCamMs = 0;
uint32_t g_lastAliveMs = 0;

// ── Link business edges (transport callbacks; main is the binder) ──────────
// WiFi command edge: maps the validated frame via the ONE command table and
// owns the LINK_CMD publish (transport must not publish: the wire type is
// NOT the CmdTarget value — kTypeCamCtl=0x03 vs CmdTarget::CAM=0).
void onWifiCommand(void* ctx, const linkcode::Frame& f) {
  (void)ctx;
  PowerCommand cmd;
  if (!wifilink::mapCmd(f.type, f.payload, f.payloadLen, cmd)) {
    return;  // unreachable: transport value-checks before invoking
  }
  EventBus::publish(HEAD_EVENT_LINK_CMD, static_cast<uint32_t>(cmd.target));
  g_power.apply(cmd);  // FACE->showFace, OLED ON/DIM/OFF, CAM on/off
}

// WiFi pairing edges: authoritative-PIN show + clear via the power manager.
void onWifiPairShow(void* ctx, const uint8_t pin6[6], uint16_t validSecs) {
  (void)ctx;
  g_power.showPairing(pin6, validSecs);
}

void onWifiPairDone(void* ctx) {
  (void)ctx;
  g_power.clearPairing("link");
}

// WiFi OTA edge: grammar + updater trigger (transport already consumed the
// seq + ACKed before this call). True = consumed.
bool onWifiOtaUrl(void* ctx, const uint8_t* url, uint16_t len) {
  (void)ctx;
  char buf[otamanifest::kUrlMax + 1];
  if (!otamanifest::copyOtaUrl(url, len, buf)) {
    return false;
  }
  if (g_ota.active()) {
    // Seq consumed + ACKed above; the in-flight download keeps running.
    LOG_EVERY_N(20, "LINK", "ota in flight, ignored");
    return true;
  }
  g_ota.startIfNewer(buf);  // own logs: versions + counters only
  return true;
}

// WiFi stills edge: camera power gate for the TCP stills pump.
bool isCameraOn(void* ctx) {
  (void)ctx;
  return g_cam.isEnabled();
}

// UART stub edge: same ONE command table as the USB CLI door.
void onStubCommand(void* ctx, const PowerCommand& cmd) {
  (void)ctx;
  EventBus::publish(HEAD_EVENT_LINK_CMD, static_cast<uint32_t>(cmd.target));
  g_power.apply(cmd);
}
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
  g_link.begin();  // PARKED stub (retained for switch-back).
  {
    StubHandlers sh;
    sh.ctx = nullptr;
    sh.onCommand = onStubCommand;
    g_link.setHandlers(sh);
  }
  g_wifi.begin();  // Live path: WiFi STA link client.
  {
    LinkHandlers wh;
    wh.ctx = nullptr;
    wh.onCommand = onWifiCommand;
    wh.onPairShow = onWifiPairShow;
    wh.onPairDone = onWifiPairDone;
    wh.onOtaUrl = onWifiOtaUrl;
    wh.isCameraOn = isCameraOn;
    g_wifi.setHandlers(wh);
  }
  g_cli.begin(&g_power);
  g_cli.attachLink(&g_wifi);      // LINK status line + `snap` still trigger.
  g_cli.attachOta(&g_ota);        // `ota <url>` pull updater (standalone).
  g_ota.attachFace(&g_display);   // Updater progress -> OLED bar/alert.
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
  g_wifi.poll();           // Live path: UDP commands/HB_ACK + TCP stills + assoc retry.
  g_wifi.heartbeatTick();  // Live path: 1Hz HB + 3-miss link-down watch.
  g_ota.poll();            // OTA body pump (no-op unless `ota <url>` started one).
  const uint32_t now = millis();
  // Camera grab suspended during OTA (OtaUpdater::active()): a JPEG grab
  // mid-flash contends PSRAM/flash cache with the Update write pump.
  if (!g_ota.active() && now - g_lastCamMs >= CFG_CAMERA_POLL_MS) {  // Wrap-safe elapsed gate.
    g_lastCamMs = now;
    g_cam.update();  // Grab-and-drop still (no-op while camera off).
  }
  HeadEvent ev;
  while (g_sub.receive(ev, 0)) {  // Non-blocking drain; logs at DEBUG.
    LOG_D("BUS", "ev %s payload=%lu", headEventName(ev.type), static_cast<unsigned long>(ev.payload));
  }
  if (now - g_lastAliveMs >= 5000) {
    g_lastAliveMs = now;
    LOG_I("HEAD", "alive cam=%s oled=%s face=%s qr=%s", camStateName(g_power.camState()),
          oledStateName(g_power.oledState()), faceName(g_power.faceId()), g_power.qrActive() ? "on" : "off");
    Diagnostics::logWatermarks("HEAD");
  }
}
