#pragma once
// Command coordinator (BEHAVIOR layer): the single dispatch point both doors
// call. CAM/OLED route to PowerManager; FACE/QR to the renderer; the rest is
// answered at the doors. Stored pointers only; no sockets.

#ifdef ARDUINO
// ── Firmware: command-table owner ────────────────────────────────────────────

#include <Arduino.h>

#include "abilities/display/display_task.h"
#include "common/logger.h"
#include "head_context.h"
#include "services/event_bus.h"
#include "services/power_manager.h"

class HeadCoordinator {
 public:
  HeadCoordinator() : power_(nullptr), display_(nullptr) {}

  // Stored, never owned. Call once in setup().
  void begin(PowerManager* power, DisplayRenderer* display) {
    power_ = power;
    display_ = display;
  }

  // THE command table: every parsed line/frame from either door lands here.
  void dispatch(const PowerCommand& cmd) {
    switch (cmd.target) {
      case CmdTarget::CAM:
      case CmdTarget::OLED:
        // Rails stay in PowerManager (single home, no forked table).
        if (power_ != nullptr) {
          power_->apply(cmd);
        }
        break;
      case CmdTarget::FACE: {
        const uint8_t id = headapply::resolveFace(cmd.arg);
        if (display_ != nullptr) {
          display_->showEyes(id);
        }
        // Status mirrors the gated id: a lower request never displaces a higher expression.
        const uint8_t active = (display_ != nullptr) ? display_->eyesId() : id;
        if (power_ != nullptr) {
          power_->setEyesId(active);
        }
        if (power_ != nullptr && power_->oledState() == OLED_OFF) {
          LOG_I("PWR", "face=%s stored (display off, paints on next on)", faceName(active));
        } else {
          LOG_I("PWR", "face=%s", faceName(active));
        }
        EventBus::publish(HEAD_EVENT_FACE_CHANGED, static_cast<uint32_t>(active));
        break;
      }
      case CmdTarget::QR:
        if (display_ == nullptr) {
          break;
        }
        if (cmd.arg != 0) {
          display_->showQrDemo();
        } else {
          display_->clearQr("cli");
        }
        break;
      case CmdTarget::STATUS:
      case CmdTarget::HELP:
      case CmdTarget::LINK:
      case CmdTarget::SNAP:
      case CmdTarget::OTA:
      case CmdTarget::INVALID:
        // Guarded, not silent.
        LOG_W("PWR", "apply ignored (no state change)");
        break;
    }
  }

  // Reporting only.
  bool qrActive() const { return display_ != nullptr && display_->qrActive(); }
  bool pairActive() const { return display_ != nullptr && display_->pairActive(); }

  void status() const { LOG_I("PWR", "qr=%s pair=%s", qrActive() ? "on" : "off", pairActive() ? "on" : "off"); }

 private:
  PowerManager* power_;
  DisplayRenderer* display_;
};

#endif  // ARDUINO
