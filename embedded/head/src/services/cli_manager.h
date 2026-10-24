#pragma once
// USB serial CLI (firmware + host-tested line parsing). Dispatches into
// HeadCoordinator::dispatch(); STATUS/HELP/LINK/SNAP/OTA answered here.
// Lowercase verbs only; no String/heap in the path.
//   cam on|off | face <id|name> | oled on|dim|off | status | link | snap |
//   ota <url> | help

#include "head_context.h"

namespace headcmd {

inline const char* skipSpaces(const char* p) {
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  return p;
}

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
  // No heap: copy the word onto a small stack buffer for lookup.
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
  if (wordIs(p, "link", 4)) {
    out.target = CmdTarget::LINK;
    out.arg = 0;
    return true;
  }
  if (wordIs(p, "snap", 4)) {
    out.target = CmdTarget::SNAP;
    out.arg = 0;
    return true;
  }
  // OTA: parseLine checks verb + non-empty tail; ota::parseUrl owns the grammar.
  if (strncmp(p, "ota", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
    const char* rest = skipSpaces(p + 3);
    if (*rest == '\0' || *rest == '\r' || *rest == '\n') {
      return false;
    }
    out.target = CmdTarget::OTA;
    out.arg = 0;
    return true;
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

// CLI fan-out sink (binder-implemented): main.cpp owns the coordinator, link,
// and OTA objects and implements this sink; the CLI only parses + triggers.
// Keeps services free of transport/abilities/behavior bindings with identical
// verbs, logs, and dispatch outcomes.
struct ICliSink {
  virtual ~ICliSink() = default;
  virtual void onStatusExtra() = 0;  // coordinator + link status lines
  virtual void onLinkStatus() = 0;   // link door status line
  virtual void onSnap() = 0;         // link still trigger
  virtual void onOta(const char* args) = 0;
  virtual void onCoord(const PowerCommand& cmd) = 0;  // CAM/FACE/OLED dispatch
};

#ifdef ARDUINO
// ── Firmware: USB serial line door ─────────────────────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"
#include "services/power_manager.h"

#ifndef CFG_CLI_LINE_LEN
#define CFG_CLI_LINE_LEN 64
#endif

class CliManager {
 public:
  CliManager() : power_(nullptr), sink_(nullptr), lineLen_(0) {}

  void begin(PowerManager* power) {
    power_ = power;
    printHelp();
  }

  // Stored, never owned. Main.cpp implements the fan-out.
  void attachSink(ICliSink* sink) { sink_ = sink; }

  // Each newline parses and dispatches.
  void poll() {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\n') {
        lineBuf_[lineLen_] = '\0';
        PowerCommand cmd;
        if (headcmd::parseLine(lineBuf_, cmd)) {
          dispatch(cmd, lineBuf_);
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
  void dispatch(const PowerCommand& cmd, const char* line) {
    if (power_ == nullptr) {
      return;
    }
    if (cmd.target == CmdTarget::STATUS) {
      power_->status();
      if (sink_ != nullptr) {
        sink_->onStatusExtra();
      }
    } else if (cmd.target == CmdTarget::HELP) {
      printHelp();
    } else if (cmd.target == CmdTarget::LINK) {
      if (sink_ != nullptr) {
        sink_->onLinkStatus();
      } else {
        LOG_W("CLI", "link not attached");
      }
    } else if (cmd.target == CmdTarget::SNAP) {
      if (sink_ != nullptr) {
        sink_->onSnap();
      } else {
        LOG_W("CLI", "link not attached");
      }
    } else if (cmd.target == CmdTarget::OTA) {
      if (sink_ != nullptr && line != nullptr) {
        const char* verb = headcmd::skipSpaces(line);
        sink_->onOta(headcmd::skipSpaces(verb + 3));
      } else {
        LOG_W("CLI", "ota not attached");
      }
    } else {
      if (sink_ != nullptr) {
        sink_->onCoord(cmd);
      }
    }
  }

  void printHelp() {
    LOG_I("CLI",
          "cmds: cam on|off | face <boot|happy|sad|blink|alert|curious|surprised|angry|scared|excited|confused|0-11> | "
          "oled on|dim|off | status | "
          "link | snap | ota "
          "<url> | help");
  }

  PowerManager* power_;
  ICliSink* sink_;
  char lineBuf_[CFG_CLI_LINE_LEN];
  size_t lineLen_;
};

#endif  // ARDUINO
