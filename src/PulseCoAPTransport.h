// PulseCoAPTransport.h
// Abstract UDP transport seam. PulseCoAPClient/Server talk to the network
// only through this interface, so swapping WiFiUDP for AsyncUDP, a raw
// lwIP socket, or a host-side test double never touches the protocol code.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pulsecoap {

// Endpoint: UDP remote address + port.
//
// Supports both IPv4 and IPv6.  When isV6 == false (the default) the
// address is in ip[4] (MSB first), matching the original layout — all
// existing code that uses ep.ip[0]…ip[3] continues to work unchanged.
// When isV6 == true the address is in v6[16] (RFC 4291, MSB first) and
// ip[4] is ignored.
//
// Transport adapters that speak both families (e.g. PosixUdpTransport with
// a dual-stack AF_INET6 socket) automatically demap incoming IPv4-mapped
// IPv6 addresses (::ffff:a.b.c.d) so callers always see plain IPv4 for IPv4
// peers and plain IPv6 for IPv6 peers.
struct Endpoint {
    uint8_t  ip[4]  = {0, 0, 0, 0};  // IPv4 address (MSB first); used when isV6==false
    uint8_t  v6[16] = {};             // IPv6 address (MSB first, RFC 4291); used when isV6==true
    bool     isV6   = false;
    uint16_t port   = 0;

    bool operator==(const Endpoint& other) const {
        if (port != other.port || isV6 != other.isV6) return false;
        if (isV6) {
            for (uint8_t i = 0; i < 16; ++i)
                if (v6[i] != other.v6[i]) return false;
            return true;
        }
        return ip[0] == other.ip[0] && ip[1] == other.ip[1] &&
               ip[2] == other.ip[2] && ip[3] == other.ip[3];
    }
    bool operator!=(const Endpoint& other) const { return !(*this == other); }
};

// Implement this against whatever UDP stack your platform provides.
class Transport {
public:
    virtual ~Transport() = default;

    // One-time setup (e.g. udp.begin(port) on Arduino). Returns false on
    // failure (port already bound, no network, etc).
    virtual bool begin(uint16_t localPort) = 0;

    // Sends one UDP datagram. Returns false if the send could not be
    // queued (this is fire-and-forget at the transport level — CoAP's own
    // reliability layer, PulseCoAPTransaction, is what handles loss).
    virtual bool send(const Endpoint& to, const uint8_t* data, size_t length) = 0;

    // Non-blocking receive: if a datagram is waiting, copies up to
    // `capacity` bytes into `buffer`, fills `from`, and returns the
    // datagram length. Returns 0 if nothing is waiting.
    virtual size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) = 0;
};

} // namespace pulsecoap
