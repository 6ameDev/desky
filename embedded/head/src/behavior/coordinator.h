#pragma once
// desky-head command coordinator — BEHAVIOR layer, header-only orchestration.
//
// ONE command table: HeadCoordinator::dispatch() is the SINGLE dispatch point
// both doors call — the USB serial CLI (lowercase verbs, laptop terminal) and
// the link handlers in main.cpp (UART stub + WiFi client) each parse a line or
// frame into a PowerCommand and land here. Narrowed from
// PowerManager::apply() (Task 6): PowerManager keeps the CAM/OLED rails +
// boot matrix + status + pairing/QR forwarders; FACE/QR routing lives here,
// next to the FaceRenderer it drives. No sockets, no business state of its
// own — stored pointers only, main owns the targets.
//
// Publishers stay at the doors (main handlers own the LINK_CMD publish, the
// CLI door answers STATUS/HELP/LINK/SNAP/OTA itself); dispatch() only routes
// effects and mirrors the FACE_CHANGED publish apply() used to own (the
// CAMERA_STATE publish stays on the CAM rail in PowerManager).

#ifdef ARDUINO
// ── Firmware: command-table owner ────────────────────────────────────────────

#include <Arduino.h>

#include "abilities/face/face_task.h"
#include "common/logger.h"
#include "head_context.h"
#include "services/event_bus.h"
#include "services/power_manager.h"

class HeadCoordinator {
 public:
  HeadCoordinator() : power_(nullptr), face_(nullptr) {}

  // Stores the rails + renderer (never owned). Call once in setup() after
  // PowerManager::begin(), before the link handlers attach.
  void begin(PowerManager* power, FaceRenderer* face) {
    power_ = power;
    face_ = face;
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
        if (power_ != nullptr) {
          power_->setFaceId(id);
        }
        if (face_ != nullptr) {
          face_->showFace(id);
        }
        if (power_ != nullptr && power_->oledState() == OLED_OFF) {
          LOG_I("PWR", "face=%s stored (display off, paints on next on)", faceName(id));
        } else {
          LOG_I("PWR", "face=%s", faceName(id));
        }
        EventBus::publish(HEAD_EVENT_FACE_CHANGED, static_cast<uint32_t>(id));
        break;
      }
      case CmdTarget::QR:
        // Bench door (`qr` = show demo, `qr clear` = clear): QR/pairing show
        // modes visible in status below. A link pairing arrives via
        // showPairing(), not here; `qr clear` clears either overlay.
        if (power_ == nullptr) {
          break;
        }
        if (cmd.arg != 0) {
          power_->showQrDemo();
        } else {
          power_->clearQr();
        }
        break;
      case CmdTarget::STATUS:
      case CmdTarget::HELP:
      case CmdTarget::LINK:
      case CmdTarget::SNAP:
      case CmdTarget::OTA:
      case CmdTarget::INVALID:
        // STATUS/HELP/LINK/SNAP/OTA are answered by the CLI door itself (LINK
        // prints the WiFi status line, SNAP queues a JPEG still, OTA starts
        // the pull updater); INVALID never reaches dispatch (doors reject at
        // parse). Guarded, not silent.
        LOG_W("PWR", "apply ignored (no state change)");
        break;
    }
  }

 private:
  PowerManager* power_;  // rails owner (main binds, never owned here)
  FaceRenderer* face_;   // overlay renderer (main binds, never owned here)
};

#endif  // ARDUINO
