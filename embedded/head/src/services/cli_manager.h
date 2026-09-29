#pragma once
// desky-head USB serial CLI — header-only, laptop-terminal door.
//
// Top half (namespace headcmd) is Arduino-free line parsing so host Unity
// tests cover it; the bottom half (class CliManager) is firmware-only
// (#ifdef ARDUINO): fixed-buffer Serial line assembly dispatching into the
// ONE command table (PowerManager::apply).
//
// Reference:
//   cam on|off            camera power gate (sensor deinit/re-init)
//   face <id>             0 boot, 1 happy, 2 sad, 3 blink, 4 alert (or name)
//   oled on|dim|off       display matrix row
//   status                power + camera stats snapshot
//   help                  this reference
// Lowercase verbs only (humans); the UART link door speaks UPPERCASE.
// No String/heap anywhere in the path (static line buffer, strncmp math).

#include "head_context.h"

namespace headcmd {

inline const char* skipSpaces(const char* p) {
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  return p;
}

// Word match + clean tail (only spaces/CR/LF/NUL may follow the word).
inline bool wordIs(const char* p, const char* word, size_t n) {
  if (strncmp(p, word, n) != 0) {
    return false;
  }
  p += n;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  return *p == '\0';
}

// "face" arg: all-digits number, else a face name; false when neither.
inline bool parseFaceArg(const char* p, int32_t& idOut) {
  p = skipSpaces(p);
  if (*p >= '0' && *p <= '9') {
    int32_t id = 0;
    while (*p >= '0' && *p <= '9') {
      id = id * 10 + (*p - '0');
      ++p;
    }
    p = skipSpaces(p);
    if (*p != '\0' && *p != '\r' && *p != '\n') {
      return false;
    }
    idOut = id;
    return true;
  }
  // Name: copy the word onto a small stack buffer (no heap) for lookup.
  char name[16];
  size_t n = 0;
  while (*p != '\0' && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t') {
    if (n + 1 >= sizeof(name)) {
      return false;
    }
    name[n++] = *p++;
  }
  name[n] = '\0';
  p = skipSpaces(p);
  if ((*p != '\0' && *p != '\r' && *p != '\n') || n == 0) {
    return false;
  }
  const uint8_t id = faceIdFromName(name);
  if (id == 0xFF) {
    return false;
  }
  idOut = id;
  return true;
}

inline bool parseLine(const char* line, PowerCommand& out) {
  if (line == nullptr) {
    return false;
  }
  const char* p = skipSpaces(line);
  if (strncmp(p, "cam", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
    p = skipSpaces(p + 3);
    if (wordIs(p, "on", 2)) {
      out.target = CmdTarget::CAM;
      out.arg = 1;
      return true;
    }
    if (wordIs(p, "off", 3)) {
      out.target = CmdTarget::CAM;
      out.arg = 0;
      return true;
    }
    return false;
  }
  if (strncmp(p, "face", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
    int32_t id = 0;
    if (!parseFaceArg(p + 4, id)) {
      return false;
    }
    out.target = CmdTarget::FACE;
    out.arg = id;
    return true;
  }
  if (strncmp(p, "oled", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
    p = skipSpaces(p + 4);
    if (wordIs(p, "on", 2)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_ON;
      return true;
    }
    if (wordIs(p, "dim", 3)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_DIM;
      return true;
    }
    if (wordIs(p, "off", 3)) {
      out.target = CmdTarget::OLED;
      out.arg = OLED_OFF;
      return true;
    }
    return false;
  }
  if (wordIs(p, "status", 6)) {
    out.target = CmdTarget::STATUS;
    out.arg = 0;
    return true;
  }
  if (wordIs(p, "help", 4)) {
    out.target = CmdTarget::HELP;
    out.arg = 0;
    return true;
  }
  return false;
}

}  // namespace headcmd

#ifdef ARDUINO
// ── Firmware: USB serial line door ─────────────────────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"
#include "services/power_manager.h"

// Fallbacks keep this header compilable if config keys ever drift; in-project
// include/config.h always wins (same pattern as core's coordinator.h).
#ifndef CFG_CLI_LINE_LEN
#define CFG_CLI_LINE_LEN 64
#endif

class CliManager {
 public:
  CliManager() : power_(nullptr), lineLen_(0) {}

  void begin(PowerManager* power) {
    power_ = power;
    printHelp();
  }

  // Drain Serial into a fixed buffer; each newline parses and dispatches.
  // STATUS/HELP are answered here; everything else lands in the ONE command
  // table (PowerManager::apply, which logs the result).
  void poll() {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\n') {
        lineBuf_[lineLen_] = '\0';
        PowerCommand cmd;
        if (headcmd::parseLine(lineBuf_, cmd)) {
          dispatch(cmd);
        } else if (lineLen_ > 0) {
          LOG_W("CLI", "unknown cmd (try help)");
        }
        lineLen_ = 0;
      } else if (c != '\r') {
        if (lineLen_ + 1 < sizeof(lineBuf_)) {
          lineBuf_[lineLen_++] = c;
        } else {
          LOG_W("CLI", "line too long, dropped");
          lineLen_ = 0;
        }
      }
    }
  }

 private:
  void dispatch(const PowerCommand& cmd) {
    if (power_ == nullptr) {
      return;
    }
    if (cmd.target == CmdTarget::STATUS) {
      power_->status();
    } else if (cmd.target == CmdTarget::HELP) {
      printHelp();
    } else {
      power_->apply(cmd);
    }
  }

  void printHelp() {
    LOG_I("CLI", "cmds: cam on|off | face <boot|happy|sad|blink|alert|0-4> | oled on|dim|off | status | help");
  }

  PowerManager* power_;
  char lineBuf_[CFG_CLI_LINE_LEN];
  size_t lineLen_;
};

#endif  // ARDUINO
