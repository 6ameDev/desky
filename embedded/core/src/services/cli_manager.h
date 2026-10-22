#pragma once
// Core USB serial CLI — header-only, minimal carrier door.
//
// The core has no interactive console otherwise: this door exists for one
// carrier op — staging the head OTA pull URL (`otahead <url>`), which the
// main-loop pump unicasts to the HB-pinned head. Lowercase verbs only.
// No String/heap anywhere in the path (static line buffer).
//
// Reference:
//   otahead <url>  stage a head pull URL (1..128B, '@' rejected) for one link
//                  unicast. The manifest version rides the POST ?version=
//                  (single source); this verb only triggers.
//   help           this reference
//
// Dispatch re-parses the raw line (the URL rides the line, not the command
// struct): parseLine only gates verb + non-empty tail, the grammar lives in
// the pure helpers below so host tests cover it.

#include <stddef.h>
#include <stdint.h>

#include "link/ota_manifest.h"

namespace corecmd {

enum class Target : uint8_t { OTAHEAD, HELP, INVALID };

// Word match + clean tail (only spaces/CR/LF/NUL may follow the word).
inline bool wordIs(const char* p, const char* word, size_t n) {
  if (p == nullptr) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (p[i] != word[i]) {
      return false;
    }
  }
  p += n;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    ++p;
  }
  return *p == '\0';
}

inline const char* skipSpaces(const char* p) {
  if (p == nullptr) {
    return nullptr;
  }
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  return p;
}

struct CoreCommand {
  Target target = Target::INVALID;
};

// Verb gate only: `otahead` needs a non-empty tail (re-parsed at dispatch),
// `help` is bare. False otherwise.
inline bool parseLine(const char* line, CoreCommand& out) {
  out.target = Target::INVALID;
  if (line == nullptr) {
    return false;
  }
  const char* p = skipSpaces(line);
  if (p == nullptr) {
    return false;
  }
  if (p[0] == 'o' && p[1] == 't' && p[2] == 'a' && p[3] == 'h' && p[4] == 'e' && p[5] == 'a' && p[6] == 'd' &&
      (p[7] == ' ' || p[7] == '\t')) {
    const char* rest = skipSpaces(p + 7);
    if (*rest == '\0' || *rest == '\r' || *rest == '\n') {
      return false;  // no URL
    }
    out.target = Target::OTAHEAD;
    return true;
  }
  if (wordIs(p, "help", 4)) {
    out.target = Target::HELP;
    return true;
  }
  return false;
}

// Split the `otahead` tail into its single URL argument. Bounded copy into
// the caller-owned 129B buffer. False on: null input, empty URL, URL over
// 128B, or any trailing token (the version rides the POST ?version=
// exclusively, so extra tokens fail closed).
inline bool parseOtaheadTail(const char* tail, char* urlOut129) {
  if (tail == nullptr || urlOut129 == nullptr) {
    return false;
  }
  const char* p = skipSpaces(tail);
  // Single token: up to the next space/CR/LF/NUL, then clean tail only.
  size_t urlLen = 0;
  while (p[urlLen] != '\0' && p[urlLen] != '\r' && p[urlLen] != '\n' && p[urlLen] != ' ' && p[urlLen] != '\t') {
    ++urlLen;
  }
  if (urlLen == 0 || urlLen > otamanifest::kUrlMax) {
    return false;
  }
  for (size_t i = 0; i < urlLen; ++i) {
    urlOut129[i] = p[i];
  }
  urlOut129[urlLen] = '\0';
  p = skipSpaces(p + urlLen);
  if (*p != '\0' && *p != '\r' && *p != '\n') {
    return false;  // trailing garbage (incl. a version token — see above)
  }
  return true;
}

}  // namespace corecmd

// OTA staging edge (binder-implemented): main.cpp owns the ability object and
// implements this sink; the CLI only parses + triggers. Keeps services free
// of ability bindings with identical verbs, logs, and dispatch outcomes.
struct ICliOtaSink {
  virtual ~ICliOtaSink() = default;
  virtual bool stageOtaUrl(const char* url) = 0;
  virtual size_t stagedOtaLen() const = 0;
};

#ifdef ARDUINO
// ── Firmware: USB serial line door ─────────────────────────────────────────

#include <Arduino.h>

#include "common/logger.h"
#include "config.h"

// Fallbacks if config keys drift; in-project include/config.h always wins.
#ifndef CFG_CLI_LINE_LEN
#define CFG_CLI_LINE_LEN 160
#endif

class CoreCli {
 public:
  CoreCli() : ota_(nullptr), lineLen_(0) {}

  void begin(ICliOtaSink* ota) {
    ota_ = ota;
    printHelp();
  }

  // Drain Serial into a fixed buffer; each newline parses and dispatches.
  void poll() {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\n') {
        lineBuf_[lineLen_] = '\0';
        corecmd::CoreCommand cmd;
        if (corecmd::parseLine(lineBuf_, cmd)) {
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
  void dispatch(const corecmd::CoreCommand& cmd, const char* line) {
    if (cmd.target == corecmd::Target::HELP) {
      printHelp();
      return;
    }
    if (cmd.target != corecmd::Target::OTAHEAD) {
      return;
    }
    if (ota_ == nullptr || line == nullptr) {
      LOG_W("CLI", "otahead not attached");
      return;
    }
    // URL rides the raw line; parseOtaheadTail owns the arg grammar, the
    // ability stage gate owns the URL byte grammar (fail-closed).
    const char* verb = corecmd::skipSpaces(line);
    char url[otamanifest::kUrlMax + 1];
    if (!corecmd::parseOtaheadTail(verb + 7, url)) {
      LOG_W("CLI", "bad otahead args (want otahead <url>)");
      return;
    }
    if (!ota_->stageOtaUrl(url)) {
      LOG_W("CLI", "otahead refused (bad url or ota not ready)");
      return;
    }
    // Length only: the manifest version is reported by the staging server
    // itself — this door never reads it.
    LOG_I("CLI", "otahead staged len=%u", static_cast<unsigned>(ota_->stagedOtaLen()));
  }

  void printHelp() { LOG_I("CLI", "cmds: otahead <url> | help"); }

  ICliOtaSink* ota_;
  char lineBuf_[CFG_CLI_LINE_LEN];
  size_t lineLen_;
};

#endif  // ARDUINO
