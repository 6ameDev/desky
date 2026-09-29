#pragma once
// desky-head talk-wire UART stub — header-only, deliberately DUMB.
//
// Top half (namespace linkstub) is Arduino-free line->verb mapping so host
// Unity tests cover it; the bottom half (class LinkStub) is firmware-only
// (#ifdef ARDUINO): UART2 @115200 on GPIO12/13, plain "AWAKE" after boot +
// periodic "HB" heartbeat TX, byte/line RX assembly mapping face/power
// verbs into the ONE command table (PowerManager::apply).
//
// Framing + CRC + the S3-waits-for-AWAKE handshake + drop-stale-frames are
// TASK 3 (shared/link codec) — this stub sends fixed lines the task-3 S3
// side can already hear, and accepts the same verbs the real codec will
// frame. Two dialects on purpose: UPPERCASE here (wire), lowercase on the
// USB CLI (human) — task 3 unifies them behind the codec.

#include "head_context.h"

namespace linkstub {

// Tail of a line after a verb: only spaces/CR/LF/NUL may follow.
inline bool tailOk(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  return *p == '\0';
}

// "FACE <n>": numeric id (clamped downstream by apply/clampFace, so any
// non-negative int parses); "CAM ON|OFF"; "OLED ON|DIM|OFF".
inline bool mapLine(const char* line, PowerCommand& out) {
  if (line == nullptr) {
    return false;
  }
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  if (strncmp(line, "FACE ", 5) == 0) {
    const char* p = line + 5;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (*p < '0' || *p > '9') {
      return false;
    }
    int32_t id = 0;
    while (*p >= '0' && *p <= '9') {
      id = id * 10 + (*p - '0');
      ++p;
    }
    if (!tailOk(p)) {
      return false;
    }
    out.target = CmdTarget::FACE;
    out.arg = id;
    return true;
  }
  if (strncmp(line, "CAM ", 4) == 0) {
    const char* p = line + 4;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (strncmp(p, "ON", 2) == 0 && tailOk(p + 2)) {
      out.target = CmdTarget::CAM;
      out.arg = 1;
      return true;
    }
    if (strncmp(p, "OFF", 3) == 0 && tailOk(p + 3)) {
      out.target = CmdTarget::CAM;
      out.arg = 0;
      return true;
    }
    return false;
  }
  if (strncmp(line, "OLED ", 5) == 0) {
    const char* p = line + 5;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (strncmp(p, "ON", 2) == 0 && tailOk(p + 2)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_ON;
      return true;
    }
    if (strncmp(p, "DIM", 3) == 0 && tailOk(p + 3)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_DIM;
      return true;
    }
    if (strncmp(p, "OFF", 3) == 0 && tailOk(p + 3)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_OFF;
      return true;
    }
    return false;
  }
  return false;
}

}  // namespace linkstub

#ifdef ARDUINO
// ── Firmware: UART2 AWAKE/heartbeat TX + verb RX ───────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"
#include "services/event_bus.h"
#include "services/power_manager.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_LINK_BAUD
#define CFG_LINK_BAUD 115200
#endif
#ifndef CFG_LINK_HEARTBEAT_MS
#define CFG_LINK_HEARTBEAT_MS 1000
#endif

class LinkStub {
 public:
  LinkStub() : power_(nullptr), lastHb_(0), lineLen_(0) {}

  void begin(PowerManager* power) {
    power_ = power;
    Serial2.begin(CFG_LINK_BAUD, SERIAL_8N1, MCU_LINK_UART_RX, MCU_LINK_UART_TX);
    lastHb_ = millis();
    sendAwake();
    LOG_I("LINK", "uart2 up tx=%d rx=%d baud=%d", MCU_LINK_UART_TX, MCU_LINK_UART_RX, CFG_LINK_BAUD);
  }

  // Plain AWAKE after boot (the S3-waits-for-AWAKE rule is consumed by the
  // task-3 codec; this line is what it will hear).
  void sendAwake() { Serial2.println("AWAKE"); }

  void heartbeatTick() {
    const uint32_t now = millis();
    if (now - lastHb_ >= CFG_LINK_HEARTBEAT_MS) {
      lastHb_ = now;
      Serial2.println("HB");
    }
  }

  // RX stub: fixed-buffer line assembly; each newline maps into the ONE
  // command table. Overlong/garbage lines are dropped with a WARN (the
  // local stand-in for task 3's drop-stale-frames rule).
  void poll() {
    while (Serial2.available() > 0) {
      const char c = static_cast<char>(Serial2.read());
      if (c == '\n') {
        lineBuf_[lineLen_] = '\0';
        PowerCommand cmd;
        if (linkstub::mapLine(lineBuf_, cmd)) {
          EventBus::publish(HEAD_EVENT_LINK_CMD, static_cast<uint32_t>(cmd.target));
          if (power_ != nullptr) {
            power_->apply(cmd);
          }
        } else if (lineLen_ > 0) {
          LOG_W("LINK", "drop garbage line");
        }
        lineLen_ = 0;
      } else if (c != '\r') {
        if (lineLen_ + 1 < sizeof(lineBuf_)) {
          lineBuf_[lineLen_++] = c;
        } else {
          LOG_W("LINK", "drop overlong line");
          lineLen_ = 0;
        }
      }
    }
  }

 private:
  PowerManager* power_;
  uint32_t lastHb_;
  char lineBuf_[64];
  size_t lineLen_;
};

#endif  // ARDUINO
