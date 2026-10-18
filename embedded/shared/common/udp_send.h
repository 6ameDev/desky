#pragma once
// Checked UDP unicast helper — shared by desky-core + desky-head.
//
// Context: ~10 UDP send sites used to ignore beginPacket/write/endPacket
// returns while logging success and bumping TX counters, overclaiming
// delivery. Every send now goes through udpSendChecked; the CALLER logs
// (WARN, release-visible) and counts — this header never logs.
//
// Structure: udpsend::isSendOk is Arduino-free pure logic (host Unity
// covered); udpSendChecked is thin Arduino socket glue (#ifdef ARDUINO).
// Namespace is `udpsend`, never `link` (POSIX ::link() collision — same
// workaround as the link headers). No heap, no String, no payload logging
// (counters + addresses only).

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFiUdp.h>
#endif

namespace udpsend {

// Send verdict (host-testable): true iff beginPacket accepted (==1), the
// full datagram landed (written==len), and the packet closed (endPacket==1).
inline bool isSendOk(int beginRet, size_t written, size_t len, int endRet) {
  return beginRet == 1 && written == len && endRet == 1;
}

#ifdef ARDUINO
// Thin checked unicast: begin -> write -> end, verdict via isSendOk above.
// No logging inside (caller logs + counts). Templated on the address so both
// call shapes work (IPAddress peers + the head's CFG_LINK_HOST literal).
template <typename Addr>
inline bool udpSendChecked(WiFiUDP& udp, const Addr& addr, uint16_t port, const uint8_t* buf, size_t len) {
  if (len > 0 && buf == nullptr) {
    return false;
  }
  const int beginRet = udp.beginPacket(addr, port);
  size_t written = 0;
  if (beginRet == 1) {
    written = udp.write(buf, len);
  }
  const int endRet = udp.endPacket();
  return isSendOk(beginRet, written, len, endRet);
}
#endif  // ARDUINO

}  // namespace udpsend
