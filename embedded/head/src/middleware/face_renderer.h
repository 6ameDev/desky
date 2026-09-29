#pragma once
// desky-head face renderer — header-only middleware, owns face semantics.
//
// SHOW_FACE{id} API, procedural face art, and the ~25fps render task live
// HERE, not in the HAL: the panel driver (hal/ssd1306_display.h) exposes
// primitives only. Face vocabulary (FaceId, names, clamp) lives Arduino-free
// in include/head_context.h so host Unity tests cover it; art here is
// procedural GFX primitives (placeholder faces — the API, not the pixels,
// is the contract).
//
// Power: IActuator bool gate (OFF = panel off + render gated) plus setDim()
// for the DIM matrix row (low refresh + low contrast). OFF gates everything;
// a face id set while off is stored and rendered on next ON.

#include "head_context.h"

#ifdef ARDUINO
// ── Firmware: face renderer ─────────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>

#include "common/iactuator.h"
#include "common/logger.h"
#include "config.h"
#include "hal/ssd1306_display.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_FACE_FPS
#define CFG_FACE_FPS 25
#endif
#ifndef CFG_FACE_FPS_DIM
#define CFG_FACE_FPS_DIM 5
#endif
#ifndef CFG_FACE_STACK_WORDS
#define CFG_FACE_STACK_WORDS 4096
#endif
#ifndef CFG_FACE_PRIORITY_OFFSET
#define CFG_FACE_PRIORITY_OFFSET 2
#endif
#ifndef CFG_FACE_CORE
#define CFG_FACE_CORE 0
#endif

class FaceRenderer : public IActuator {
 public:
  explicit FaceRenderer(Ssd1306Display& panel) : panel_(panel) {}

  // Panel init owns the absent-OLED policy (never asserts); the renderer
  // then paints the banner + boot face and starts the render task.
  bool init() override {
    if (!panel_.init()) {
      return false;
    }
    enabled_ = true;
    if (panel_.present()) {
      panel_.gfx().setTextSize(1);
      panel_.gfx().setTextColor(SSD1306_WHITE);
      panel_.gfx().setCursor(0, 0);
      panel_.gfx().print(F("desky-head"));
      panel_.gfx().display();
      showFace(FACE_BOOT);
      xTaskCreatePinnedToCore(&FaceRenderer::renderTask, "face", CFG_FACE_STACK_WORDS, this,
                              tskIDLE_PRIORITY + CFG_FACE_PRIORITY_OFFSET, nullptr, CFG_FACE_CORE);
    }
    return true;
  }

  void setPowerState(bool enable) override {
    enabled_ = enable;
    panel_.setPowerState(enable);
    if (enable) {
      drawFace(faceId_);
    }
  }

  bool isEnabled() const override { return enabled_; }

  bool present() const { return panel_.present(); }

  // SHOW_FACE{id} API: stores (clamped) and paints immediately when live;
  // the render task repaints at the matrix cadence either way.
  void showFace(uint8_t id) {
    faceId_ = clampFace(static_cast<int>(id));
    if (panel_.present() && enabled_) {
      drawFace(faceId_);
    }
  }

  uint8_t faceId() const { return faceId_; }

  // DIM matrix row: low refresh (task reads dim_ each loop) + low contrast.
  void setDim(bool dim) {
    dim_ = dim;
    if (panel_.present() && enabled_) {
      panel_.setContrast(dim ? Ssd1306Display::kContrastDim : Ssd1306Display::kContrastOn);
    }
  }

  bool isDim() const { return dim_; }

 private:
  static void renderTask(void* arg) {
    auto* self = static_cast<FaceRenderer*>(arg);
    for (;;) {
      // Never WDT-subscribed, so no feed needed; never halts.
      if (self->enabled_ && self->panel_.present()) {
        self->drawFace(self->faceId_);
      }
      const uint32_t periodMs = 1000u / (self->dim_ ? CFG_FACE_FPS_DIM : CFG_FACE_FPS);
      vTaskDelay(pdMS_TO_TICKS(periodMs));
    }
  }

  void drawFace(uint8_t id) {
    if (!panel_.present()) {
      return;
    }
    auto& display = panel_.gfx();
    display.clearDisplay();
    switch (id) {
      case FACE_HAPPY:
        display.fillCircle(44, 24, 6, SSD1306_WHITE);
        display.fillCircle(84, 24, 6, SSD1306_WHITE);
        arc(display, 64, 38, 12, 25, 155);  // smile
        break;
      case FACE_SAD:
        display.fillCircle(44, 24, 4, SSD1306_WHITE);
        display.fillCircle(84, 24, 4, SSD1306_WHITE);
        arc(display, 64, 58, 12, 205, 335);  // frown
        break;
      case FACE_BLINK:
        display.drawLine(38, 24, 50, 24, SSD1306_WHITE);
        display.drawLine(78, 24, 90, 24, SSD1306_WHITE);
        display.drawLine(56, 48, 72, 48, SSD1306_WHITE);
        break;
      case FACE_ALERT:
        display.drawCircle(44, 24, 8, SSD1306_WHITE);
        display.drawCircle(84, 24, 8, SSD1306_WHITE);
        display.fillCircle(44, 24, 2, SSD1306_WHITE);
        display.fillCircle(84, 24, 2, SSD1306_WHITE);
        display.drawCircle(64, 48, 5, SSD1306_WHITE);
        break;
      case FACE_BOOT:
      default:
        display.fillRoundRect(36, 16, 16, 16, 3, SSD1306_WHITE);
        display.fillRoundRect(76, 16, 16, 16, 3, SSD1306_WHITE);
        display.drawLine(54, 48, 74, 48, SSD1306_WHITE);
        break;
    }
    display.setCursor(0, 56);
    display.print(faceName(id));
    display.display();
  }

  // Pixel arc for smile/frown (degrees, 0 = +x, 90 = +y down the panel).
  static void arc(Adafruit_SSD1306& display, int cx, int cy, int r, int a0, int a1) {
    for (int a = a0; a <= a1; a += 5) {
      const float rad = static_cast<float>(a) * 3.14159265f / 180.0f;
      display.drawPixel(cx + static_cast<int>(r * cosf(rad)), cy + static_cast<int>(r * sinf(rad)), SSD1306_WHITE);
    }
  }

  Ssd1306Display& panel_;
  volatile bool enabled_ = false;
  volatile bool dim_ = false;
  volatile uint8_t faceId_ = FACE_BOOT;
};

#endif  // ARDUINO
