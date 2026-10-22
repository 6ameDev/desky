#pragma once
// Checked UDP unicast helper. Every send goes through udpSendChecked; the caller logs
// and counts, this header never logs. Namespace is udpsend, never link (POSIX ::link() collision).

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFiUdp.h>
#endif

namespace udpsend {

inline bool isSendOk(int beginRet, size_t written, size_t len, int endRet) {
  return beginRet == 1 && written == len && endRet == 1;
}

#ifdef ARDUINO
// Templated on the address: IPAddress peers and host literals both work.
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
