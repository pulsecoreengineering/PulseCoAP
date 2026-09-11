// PulseCoAPTransportArduinoUDP.h
// Transport adapter over Arduino's UDP base class (WiFiUDP on ESP32/ESP8266,
// EthernetUDP, etc — anything derived from the Arduino core's `UDP`).
// Only compiled when building under the Arduino framework, so it never
// pollutes a host-side test build (see test/test_message_codec.cpp, which
// links only PulseCoAPMessage.cpp and needs no Arduino headers at all).
#pragma once

#ifdef ARDUINO

#include <Udp.h>

#include "PulseCoAPTransport.h"

namespace pulsecoap {

// Wraps a caller-owned Arduino `UDP` instance (WiFiUDP, AsyncUDP's blocking
// shim, EthernetUDP, ...). PulseCoAP never constructs the underlying UDP
// object itself — you choose which stack to link.
class ArduinoUdpTransport : public Transport {
public:
    explicit ArduinoUdpTransport(UDP& udp) : udp_(udp) {}

    bool begin(uint16_t localPort) override {
        return udp_.begin(localPort) != 0;
    }

    bool send(const Endpoint& to, const uint8_t* data, size_t length) override {
        IPAddress addr(to.ip[0], to.ip[1], to.ip[2], to.ip[3]);
        if (udp_.beginPacket(addr, to.port) == 0) return false;
        size_t written = udp_.write(data, length);
        if (!udp_.endPacket()) return false;
        return written == length;
    }

    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override {
        int packetSize = udp_.parsePacket();
        if (packetSize <= 0) return 0;

        IPAddress remoteIp = udp_.remoteIP();
        from.ip[0] = remoteIp[0];
        from.ip[1] = remoteIp[1];
        from.ip[2] = remoteIp[2];
        from.ip[3] = remoteIp[3];
        from.port = udp_.remotePort();

        int toRead = packetSize < static_cast<int>(capacity) ? packetSize : static_cast<int>(capacity);
        int actuallyRead = udp_.read(buffer, toRead);
        return actuallyRead > 0 ? static_cast<size_t>(actuallyRead) : 0;
    }

private:
    UDP& udp_;
};

} // namespace pulsecoap

#endif // ARDUINO
