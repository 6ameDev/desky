#pragma once
// Captive portal — services plumbing, served in every AP session: first
// boot lands on setup, the forced-AP latch on management, STA failover on
// recovery. AsyncWebServer (port 80) + DNS hijack (DNSServer is
// Arduino-builtin, no new lib_deps). Store/forget write NVS atomically
// behind the validity flag and relay through the core<->head link; STORED
// reports only after the head sequence-ACKs, inside a bounded wait that
// covers the link retry budget. The AP passphrase never appears here: it
// lives on the printed on-body label.
//
// Top half (namespace portal) is Arduino-free pure logic so host tests
// include this header directly. Bottom half (class CaptivePortal) is
// firmware-only (async handlers, String confined to the handler boundary).
// The link arrives as an injected function-pointer sink, so this header
// takes no transport types and the services layer stays flat.

#include <stddef.h>
#include <stdint.h>

#include "services/wifi_credentials.h"

namespace portal {

constexpr const char* kRouteRoot = "/";
constexpr const char* kRouteStore = "/store";
constexpr const char* kRouteForget = "/forget";
constexpr const char* kRouteStatus = "/status";

// WPA2 floor: an empty passphrase means open network, otherwise 8..64.
constexpr size_t kPassMin = 8;

enum class Mode : uint8_t { kSetup, kManage, kRecovery };
enum class Op : uint8_t { kNone, kStore, kClear };
enum class Relay : uint8_t { kIdle, kNoLink, kRelaying, kStored, kFailed };

// Relay wait mirrors the link credential-push budget (attempts x spacing)
// plus margin; STORED is reported only inside this window.
constexpr uint8_t kRelayAttempts = 5;
constexpr uint32_t kRelayRetryMs = 1000;
constexpr uint32_t kRelayWaitMs = kRelayAttempts * kRelayRetryMs + 2000;

// POST validation: non-empty SSID within bounds, empty-or-8+ passphrase.
inline bool storeValid(size_t ssidLen, size_t passLen) {
  if (ssidLen == 0 || ssidLen > wificreds::kSsidMax) {
    return false;
  }
  if (passLen == 0) {
    return true;
  }
  return passLen >= kPassMin && passLen <= wificreds::kPassMax;
}

// Bounded-wait expiry. Unsigned subtraction: wrap-safe across rollover.
inline bool relayTimedOut(uint32_t startMs, uint32_t nowMs, uint32_t waitMs = kRelayWaitMs) {
  return (nowMs - startMs) >= waitMs;
}

inline const char* modeTitle(Mode m) {
  switch (m) {
    case Mode::kSetup:
      return "First-time setup";
    case Mode::kManage:
      return "Manage WiFi";
    case Mode::kRecovery:
      return "Recovery";
  }
  return "WiFi";
}

inline const char* relayText(Relay r) {
  switch (r) {
    case Relay::kIdle:
      return "idle";
    case Relay::kNoLink:
      return "link required";
    case Relay::kRelaying:
      return "relaying";
    case Relay::kStored:
      return "stored";
    case Relay::kFailed:
      return "failed";
  }
  return "idle";
}

inline const char* opText(Op o) {
  switch (o) {
    case Op::kNone:
      return "none";
    case Op::kStore:
      return "store";
    case Op::kClear:
      return "clear";
  }
  return "none";
}

// Escapes & < > " for page bodies. Always NUL-terminates when cap > 0;
// stops before a partial entity on truncation (never emits a raw <).
// Returns bytes written excluding NUL. Null in reads back empty.
inline size_t escapeHtml(const char* in, char* out, size_t cap) {
  size_t n = 0;
  if (out == nullptr || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return 0;
  }
  for (size_t i = 0; in[i] != '\0'; ++i) {
    const char* esc = nullptr;
    size_t escLen = 1;
    switch (in[i]) {
      case '&':
        esc = "&amp;";
        escLen = 5;
        break;
      case '<':
        esc = "&lt;";
        escLen = 4;
        break;
      case '>':
        esc = "&gt;";
        escLen = 4;
        break;
      case '"':
        esc = "&quot;";
        escLen = 6;
        break;
      default:
        break;
    }
    if (n + escLen >= cap) {
      break;
    }
    if (esc == nullptr) {
      out[n++] = in[i];
    } else {
      for (size_t k = 0; k < escLen; ++k) {
        out[n++] = esc[k];
      }
    }
  }
  out[n] = '\0';
  return n;
}

// Status header body: "stored: <ssid>" (escaped) or "stored: none". Takes
// no passphrase, so none can ever render.
inline size_t statusHeader(char* out, size_t cap, bool hasStored, const char* ssid) {
  size_t n = 0;
  if (out == nullptr || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  const char* prefix = "stored: ";
  for (size_t i = 0; prefix[i] != '\0' && n + 1 < cap; ++i) {
    out[n++] = prefix[i];
  }
  if (!hasStored || ssid == nullptr || ssid[0] == '\0') {
    const char* none = "none";
    for (size_t i = 0; none[i] != '\0' && n + 1 < cap; ++i) {
      out[n++] = none[i];
    }
  } else {
    char esc[wificreds::kSsidMax * 6 + 1];
    escapeHtml(ssid, esc, sizeof(esc));
    for (size_t i = 0; esc[i] != '\0' && n + 1 < cap; ++i) {
      out[n++] = esc[i];
    }
  }
  out[n] = '\0';
  return n;
}

}  // namespace portal

#ifdef ARDUINO

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#include "common/logger.h"
#include "services/config_store.h"

// Injected link edge: the portal queries liveness and triggers the sealed
// relay through these (wired in main.cpp onto the link endpoint), so this
// header takes no transport types.
struct PortalLinkSink {
  bool (*isLinked)(void* ctx);
  bool (*relayStore)(void* ctx);
  bool (*relayClear)(void* ctx);
  bool (*relayAcked)(void* ctx);
  void* ctx;
};

class CaptivePortal {
 public:
  static constexpr uint16_t kPort = 80;
  static constexpr uint16_t kDnsPort = 53;

  CaptivePortal() : server_(kPort) {}

  // Starts the portal on the AP interface (apIp is the transport's
  // softAPIP, passed in — services take no WiFi types): routes + DNS
  // hijack + listen. False when the sink is incomplete (fail-closed).
  // Call once per boot, only while the AP is up.
  bool begin(portal::Mode mode, const PortalLinkSink& sink, const IPAddress& apIp) {
    if (sink.isLinked == nullptr || sink.relayStore == nullptr || sink.relayClear == nullptr ||
        sink.relayAcked == nullptr) {
      return false;
    }
    if (running_) {
      return true;
    }
    mode_ = mode;
    sink_ = sink;
    server_.on(portal::kRouteRoot, HTTP_GET, [this](AsyncWebServerRequest* req) { handleRoot(req); });
    server_.on(portal::kRouteStore, HTTP_POST, [this](AsyncWebServerRequest* req) { handleStore(req); });
    server_.on(portal::kRouteForget, HTTP_POST, [this](AsyncWebServerRequest* req) { handleForget(req); });
    server_.on(portal::kRouteStatus, HTTP_GET, [this](AsyncWebServerRequest* req) { handleStatus(req); });
    server_.onNotFound([this](AsyncWebServerRequest* req) {
      if (req != nullptr) {
        req->redirect(portal::kRouteRoot);
      }
    });
    dns_.start(kDnsPort, "*", apIp);
    server_.begin();
    refreshStored();
    running_ = true;
    LOG_I("BOOT", "portal ready mode=%s port=%u", portal::modeTitle(mode_), static_cast<unsigned>(kPort));
    return true;
  }

  // Pumps DNS + resolves the relay latch (ACK -> stored, budget over ->
  // failed). Non-blocking; call every loop.
  void poll() {
    if (!running_) {
      return;
    }
    dns_.processNextRequest();
    if (relay_ != portal::Relay::kRelaying) {
      return;
    }
    if (sink_.relayAcked(sink_.ctx)) {
      relay_ = (op_ == portal::Op::kStore) ? portal::Relay::kStored : portal::Relay::kIdle;
      LOG_I("PORTAL", "relay confirmed op=%s", portal::opText(op_));
      op_ = portal::Op::kNone;
      return;
    }
    if (portal::relayTimedOut(relayStartMs_, millis())) {
      relay_ = portal::Relay::kFailed;
      op_ = portal::Op::kNone;
      LOG_W("PORTAL", "relay timeout, retry from the portal");
    }
  }

  bool running() const { return running_; }
  portal::Mode mode() const { return mode_; }
  portal::Relay relay() const { return relay_; }

 private:
  bool linkedNow() const { return sink_.isLinked != nullptr && sink_.isLinked(sink_.ctx); }

  // Reloads the status-header state from NVS. The passphrase loads only to
  // satisfy the loader signature and is never retained or logged.
  void refreshStored() {
    char ssid[wificreds::kSsidMax + 1] = {};
    char pass[wificreds::kPassMax + 1] = {};
    hasStored_ = ConfigStore::loadWifiCredentials(ssid, sizeof(ssid), pass, sizeof(pass));
    for (size_t i = 0; i < sizeof(pass); ++i) {
      pass[i] = 0;
    }
    size_t i = 0;
    while (ssid[i] != '\0' && i + 1 < sizeof(storedSsid_)) {
      storedSsid_[i] = ssid[i];
      ++i;
    }
    storedSsid_[i] = '\0';
    if (!hasStored_) {
      storedSsid_[0] = '\0';
    }
  }

  void handleRoot(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    const bool linked = linkedNow();
    const char* intro = (mode_ == portal::Mode::kSetup)    ? "Enter your home WiFi:"
                        : (mode_ == portal::Mode::kManage) ? "Replace or forget the stored network:"
                                                           : "Home network unreachable. Enter a new one:";
    char header[224];
    portal::statusHeader(header, sizeof(header), hasStored_, storedSsid_);
    String page = "<!doctype html><html><body><h1>desky ";
    page += portal::modeTitle(mode_);
    page += "</h1><p>";
    page += intro;
    page += "</p><p>";
    page += header;
    page += "</p>";
    if (!linked) {
      page += "<p>Head link required before storing. Keep the head powered and linked, then retry.</p>";
    } else {
      page += "<form method=\"POST\" action=\"";
      page += portal::kRouteStore;
      page +=
          "\">SSID <input name=\"ssid\" maxlength=\"32\"> Pass <input type=\"password\" name=\"pass\" "
          "maxlength=\"64\"><input type=\"submit\" value=\"Store\"></form>";
      if (hasStored_) {
        page += "<form method=\"POST\" action=\"";
        page += portal::kRouteForget;
        page += "\"><input type=\"submit\" value=\"Forget\"></form>";
      }
    }
    page += "</body></html>";
    req->send(200, "text/html", page);
  }

  // Store is link-gated: NVS writes only while the head is live, the sealed
  // relay fires, and the refreshing page reports via /status. Never blocks.
  void handleStore(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    const String ssid = req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : String();
    const String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : String();
    if (!portal::storeValid(ssid.length(), pass.length())) {
      req->send(400, "text/plain", "bad ssid/pass: ssid 1..32 chars, pass empty or 8..64");
      return;
    }
    if (!linkedNow()) {
      relay_ = portal::Relay::kNoLink;
      req->send(200, "text/html",
                "<!doctype html><html><body><p>Head link required. Keep the head powered and linked, then retry.</p>"
                "</body></html>");
      return;
    }
    if (!ConfigStore::saveWifiCredentials(ssid.c_str(), pass.c_str())) {
      req->send(500, "text/plain", "nvs write failed, retry");
      return;
    }
    refreshStored();
    op_ = portal::Op::kStore;
    relay_ = portal::Relay::kRelaying;
    relayStartMs_ = millis();
    const bool sent = sink_.relayStore(sink_.ctx);
    LOG_I("PORTAL", "store ssid=%s relay=%u", ssid.c_str(), sent ? 1u : 0u);
    String page = "<!doctype html><html><head><meta http-equiv=\"refresh\" content=\"2;url=";
    page += portal::kRouteStatus;
    page += "\"></head><body><p>Relaying to head. On STORED, reboot to join the new network.</p></body></html>";
    req->send(200, "text/html", page);
  }

  void handleForget(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    ConfigStore::clearWifiCredentials();
    refreshStored();
    if (!linkedNow()) {
      req->send(200, "text/html",
                "<!doctype html><html><body><p>Forgotten on core. Head not linked: its copy clears on the next "
                "link.</p></body></html>");
      return;
    }
    op_ = portal::Op::kClear;
    relay_ = portal::Relay::kRelaying;
    relayStartMs_ = millis();
    const bool sent = sink_.relayClear(sink_.ctx);
    LOG_I("PORTAL", "forget relay=%u", sent ? 1u : 0u);
    String page = "<!doctype html><html><head><meta http-equiv=\"refresh\" content=\"2;url=";
    page += portal::kRouteStatus;
    page += "\"></head><body><p>Clearing on head. Reboot to re-provision.</p></body></html>";
    req->send(200, "text/html", page);
  }

  // Plain status endpoint behind the refreshing pages: mode + relay latch +
  // live link + stored header, text only.
  void handleStatus(AsyncWebServerRequest* req) {
    if (req == nullptr) {
      return;
    }
    if (relay_ == portal::Relay::kIdle && !linkedNow()) {
      relay_ = portal::Relay::kNoLink;
    }
    char header[224];
    portal::statusHeader(header, sizeof(header), hasStored_, storedSsid_);
    String body = "mode=";
    body += portal::modeTitle(mode_);
    body += " relay=";
    body += portal::relayText(relay_);
    body += " linked=";
    body += linkedNow() ? "1" : "0";
    body += " ";
    body += header;
    req->send(200, "text/plain", body);
  }

  AsyncWebServer server_;
  DNSServer dns_;
  bool running_ = false;
  portal::Mode mode_ = portal::Mode::kSetup;
  PortalLinkSink sink_ = {nullptr, nullptr, nullptr, nullptr, nullptr};
  portal::Relay relay_ = portal::Relay::kIdle;
  portal::Op op_ = portal::Op::kNone;
  uint32_t relayStartMs_ = 0;
  char storedSsid_[wificreds::kSsidMax + 1] = {};
  bool hasStored_ = false;
};

#endif  // ARDUINO
