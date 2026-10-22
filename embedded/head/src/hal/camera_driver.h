#pragma once
// OV3660 camera driver (behind ISensor), header-only HAL.
// Grab-and-drop stills + stats; power-gated via setPowerState.
// Camera works on-MB (ribbon only), unlike the OLED.

#include <stdint.h>

namespace headcam {

// Millifps = frames*1000/elapsedMs; zero elapsed yields zero.
inline uint32_t millifps(uint32_t frames, uint32_t elapsedMs) {
  if (elapsedMs == 0) {
    return 0;
  }
  return (frames * 1000u) / elapsedMs;
}

}  // namespace headcam

#ifdef ARDUINO
// ── Firmware: OV3660 QVGA JPEG driver ──────────────────────────────────────

#include <Arduino.h>
#include <esp_camera.h>

#include "common/fault_manager.h"
#include "common/isensor.h"
#include "common/logger.h"
#include "config.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_CAMERA_POLL_MS
#define CFG_CAMERA_POLL_MS 500
#endif
#ifndef CFG_CAMERA_JPEG_QUALITY
#define CFG_CAMERA_JPEG_QUALITY 12
#endif

class CameraDriver : public ISensor {
 public:
  CameraDriver()
      : enabled_(false), frames_(0), dropped_(0), lastSize_(0), fpsMilli_(0), fpsWindowStart_(0), fpsWindowFrames_(0) {}

  bool init() override {
    const bool ok = initSensor();
    if (ok) {
      LOG_I("CAM", "up qvga jpeg q=%d psram=%d", CFG_CAMERA_JPEG_QUALITY, psramFound() ? 1 : 0);
    }
    return ok;
  }

  // A disabled camera returns immediately (no sensor traffic).
  void update() override {
    if (!enabled_) {
      return;
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      ++dropped_;
      return;
    }
    lastSize_ = fb->len;
    ++frames_;
    ++fpsWindowFrames_;
    const uint32_t now = millis();
    if (now - fpsWindowStart_ >= 2000) {
      fpsMilli_ = headcam::millifps(fpsWindowFrames_, now - fpsWindowStart_);
      fpsWindowFrames_ = 0;
      fpsWindowStart_ = now;
    }
    esp_camera_fb_return(fb);
  }

  void setPowerState(bool enable) override {
    if (enable == enabled_) {
      return;
    }
    if (!enable) {
      esp_camera_deinit();
      enabled_ = false;
      LOG_I("CAM", "off (sensor deinit, captures gated)");
      return;
    }
    if (initSensor()) {
      LOG_I("CAM", "on (sensor re-init)");
    } else {
      LOG_W("CAM", "re-init failed, staying off");
    }
  }

  bool isEnabled() const override { return enabled_; }
  uint32_t getTargetIntervalMs() const override { return CFG_CAMERA_POLL_MS; }

  uint32_t frames() const { return frames_; }
  uint32_t dropped() const { return dropped_; }
  size_t lastSizeBytes() const { return lastSize_; }
  uint32_t fpsMilli() const { return fpsMilli_; }

 private:
  // Board-fixed AI-Thinker bus, QVGA JPEG, single frame buffer.
  bool initSensor() {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = MCU_CAM_PIN_Y2;
    cfg.pin_d1 = MCU_CAM_PIN_Y3;
    cfg.pin_d2 = MCU_CAM_PIN_Y4;
    cfg.pin_d3 = MCU_CAM_PIN_Y5;
    cfg.pin_d4 = MCU_CAM_PIN_Y6;
    cfg.pin_d5 = MCU_CAM_PIN_Y7;
    cfg.pin_d6 = MCU_CAM_PIN_Y8;
    cfg.pin_d7 = MCU_CAM_PIN_Y9;
    cfg.pin_xclk = MCU_CAM_PIN_XCLK;
    cfg.pin_pclk = MCU_CAM_PIN_PCLK;
    cfg.pin_vsync = MCU_CAM_PIN_VSYNC;
    cfg.pin_href = MCU_CAM_PIN_HREF;
    cfg.pin_sccb_sda = MCU_CAM_PIN_SIOD;
    cfg.pin_sccb_scl = MCU_CAM_PIN_SIOC;
    cfg.pin_pwdn = MCU_CAM_PIN_PWDN;
    cfg.pin_reset = MCU_CAM_PIN_RESET;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = FRAMESIZE_QVGA;
    cfg.jpeg_quality = CFG_CAMERA_JPEG_QUALITY;
    cfg.fb_count = 1;  // Load-bearing: never raise without re-proving OTA + snap.
    const esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
      LOG_E("CAM", "esp_camera_init failed err=%d", static_cast<int>(err));
      enabled_ = false;
      return false;
    }
    enabled_ = true;
    fpsWindowStart_ = millis();
    fpsWindowFrames_ = 0;
    return true;
  }

  bool enabled_;
  uint32_t frames_;
  uint32_t dropped_;
  size_t lastSize_;
  uint32_t fpsMilli_;
  uint32_t fpsWindowStart_;
  uint32_t fpsWindowFrames_;
};

#endif  // ARDUINO
