#include <Arduino.h>

#include "abilities/face/face_task.h"
#include "abilities/ota/ota_task.h"
#include "abilities/pairing/pairing_task.h"
#include "behavior/coordinator.h"
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
#include "services/cli_manager.h"
#include "services/event_bus.h"
#include "services/power_manager.h"
#include "transport/link_client.h"
#include "transport/link_codec.h"
#include "transport/link_stub.h"

// desky-head (face-unit stub, PARKED).
// Setup order: logger -> fault -> config -> banner.
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
HeadCoordinator g_coordinator;
PairingManager g_pairing;
LinkStub g_link;
LinkClient g_wifi;
OtaUpdater g_ota;
CliManager g_cli;
// Binder fan-out: the CLI parses + triggers through this sink; the client
// objects stay owned here in main.cpp.
struct HeadCliSink : ICliSink {
  void onStatusExtra() override {
    g_coordinator.status();
    g_wifi.logStatus();
  }
  void onLinkStatus() override { g_wifi.logStatus(); }
  void onSnap() override { g_wifi.requestStill(); }
  void onOta(const char* args) override { g_ota.start(args); }
  void onCoord(const PowerCommand& cmd) override { g_coordinator.dispatch(cmd); }
};
HeadCliSink g_cliSink;
EventBus::Subscription g_sub;
uint32_t g_lastCamMs = 0;
uint32_t g_lastAliveMs = 0;

// Link callbacks (transport invokes, main executes).
void onWifiCommand(void* ctx, const linkcode::Frame& f) {
  (void)ctx;
  PowerCommand cmd;
  if (!wifilink::mapCmd(f.type, f.payload, f.payloadLen, cmd)) {
    return;
  }
  EventBus::publish(HEAD_EVENT_LINK_CMD, static_cast<uint32_t>(cmd.target));
  g_coordinator.dispatch(cmd);
}

// WiFi OTA edge (transport seq-gated + ACKed). True = consumed.
bool onWifiOtaUrl(void* ctx, const uint8_t* url, uint16_t len) {
  (void)ctx;
  char buf[otamanifest::kUrlMax + 1];
  if (!otamanifest::copyOtaUrl(url, len, buf)) {
    return false;
  }
  if (g_ota.active()) {
    LOG_EVERY_N(20, "LINK", "ota in flight, ignored");
    return true;
  }
  g_ota.startIfNewer(buf);
  return true;
}

// Camera gate for the TCP stills pump.
bool isCameraOn(void* ctx) {
  (void)ctx;
  return g_cam.isEnabled();
}

// UART stub edge.
void onStubCommand(void* ctx, const PowerCommand& cmd) {
  (void)ctx;
  EventBus::publish(HEAD_EVENT_LINK_CMD, static_cast<uint32_t>(cmd.target));
  g_coordinator.dispatch(cmd);
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
  g_i2c.scanBus();
  // esp_camera_deinit() leaves the ISR service installed; later inits log
  // "GPIO isr service already installed" at ERROR and succeed anyway.
  LogFilter::addRule("GPIO isr service already installed");
  LogFilter::begin();
  LOG_I("LOG", "filter rules=%u", LogFilter::ruleCount());
  const bool camOk = g_cam.init();
  DESKY_ASSERT(camOk);  // Camera works on-MB (ribbon only), unlike the OLED.
  const bool oledOk = g_display.init();
  (void)oledOk;  // Absent OLED is the normal on-MB state; never assert.
  g_power.begin(&g_cam, &g_display);
  g_coordinator.begin(&g_power, &g_display);
  g_pairing.attach(&g_display.overlays());
  g_link.begin();
  {
    StubHandlers sh;
    sh.ctx = nullptr;
    sh.onCommand = onStubCommand;
    g_link.setHandlers(sh);
  }
  g_wifi.begin();  // Live path: WiFi STA link client.
  {
    LinkHandlers wh;
    wh.ctx = &g_pairing;
    wh.onCommand = onWifiCommand;
    wh.onPairShow = pairingtask::onPairShow;
    wh.onPairDone = pairingtask::onPairDone;
    wh.onOtaUrl = onWifiOtaUrl;
    wh.isCameraOn = isCameraOn;
    g_wifi.setHandlers(wh);
  }
  g_cli.begin(&g_power);
  g_cli.attachSink(&g_cliSink);
  g_ota.attachFace(&g_display);
  g_sub = EventBus::subscribe();
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
  g_cli.poll();  // USB door (human).
  g_link.poll();
  g_link.heartbeatTick();
  g_wifi.poll();
  g_wifi.heartbeatTick();
  g_ota.poll();
  const uint32_t now = millis();
  // Grab suspended during OTA: JPEG grab mid-flash contends PSRAM/flash cache.
  if (!g_ota.active() && now - g_lastCamMs >= CFG_CAMERA_POLL_MS) {
    g_lastCamMs = now;
    g_cam.update();
  }
  HeadEvent ev;
  while (g_sub.receive(ev, 0)) {
    LOG_D("BUS", "ev %s payload=%lu", headEventName(ev.type), static_cast<unsigned long>(ev.payload));
  }
  if (now - g_lastAliveMs >= 5000) {
    g_lastAliveMs = now;
    LOG_I("HEAD", "alive cam=%s oled=%s face=%s qr=%s pair=%s", camStateName(g_power.camState()),
          oledStateName(g_power.oledState()), faceName(g_power.faceId()), g_coordinator.qrActive() ? "on" : "off",
          g_coordinator.pairActive() ? "on" : "off");
    Diagnostics::logWatermarks("HEAD");
  }
}
