// PulseCoAPTransportPosix.h
// Transport adapter over POSIX/BSD datagram (UDP) sockets.
//
// Suitable for:
//   - Host-side integration tests and tooling on Linux / macOS
//   - Linux-based CoAP gateways and border routers
//   - POSIX-compliant RTOS targets (Zephyr with POSIX API, NuttX, RIOT, etc.)
//
// IPv4 and IPv6
// -------------
// begin() opens a dual-stack AF_INET6 socket (IPV6_V6ONLY=0) on Linux and
// macOS, so a single socket handles both families.  If the kernel does not
// support IPv6 (or if IPv6 is disabled at build time with -DNO_IPV6) begin()
// falls back to a plain AF_INET socket.
//
// send() accepts both IPv4 (Endpoint::isV6 == false) and IPv6
// (Endpoint::isV6 == true) targets.  When the socket is dual-stack an IPv4
// target is sent via its IPv4-mapped address (::ffff:a.b.c.d).
//
// receive() fills the Endpoint from the arriving datagram; incoming IPv4
// datagrams that arrive as IPv4-mapped addresses on a dual-stack socket are
// automatically demapped to plain IPv4 (isV6 = false, ip[4] filled).
//
// Non-blocking
// ------------
// The socket is put in O_NONBLOCK mode, so receive() never stalls when the
// queue is empty — consistent with the poll()-driven Transport contract.
//
// Build:
//   g++ -std=c++11 -Isrc your_app.cpp src/PulseCoAPMessage.cpp
//       src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp src/PulseCoAPTransaction.cpp
//   (No extra library flags beyond the standard C++ runtime.)
#pragma once

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__) || \
    defined(_POSIX_VERSION)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "PulseCoAPTransport.h"

namespace pulsecoap {

class PosixUdpTransport : public Transport {
public:
    PosixUdpTransport() : fd_(-1), dualStack_(false) {}

    ~PosixUdpTransport() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    // Binds to INADDR_ANY / in6addr_any on localPort.
    // Pass 0 to get an ephemeral port (read it back with localPort()).
    bool begin(uint16_t localPort) override {
        if (fd_ >= 0) return false; // already open

        // Try dual-stack IPv6 first; fall back to IPv4-only if unavailable.
#ifndef NO_IPV6
        fd_ = ::socket(AF_INET6, SOCK_DGRAM, 0);
        if (fd_ >= 0) {
            int off = 0;
            ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
            dualStack_ = true;
        }
#endif
        if (fd_ < 0) {
            fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd_ < 0) return false;
            dualStack_ = false;
        }

        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(fd_); fd_ = -1;
            return false;
        }

        if (dualStack_) {
            struct sockaddr_in6 addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            addr.sin6_addr   = in6addr_any;
            addr.sin6_port   = htons(localPort);
            if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                       sizeof(addr)) < 0) {
                ::close(fd_); fd_ = -1;
                return false;
            }
        } else {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port        = htons(localPort);
            if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                       sizeof(addr)) < 0) {
                ::close(fd_); fd_ = -1;
                return false;
            }
        }
        return true;
    }

    bool send(const Endpoint& to, const uint8_t* data, size_t length) override {
        if (fd_ < 0) return false;
        ssize_t sent;

        if (dualStack_) {
            struct sockaddr_in6 addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            addr.sin6_port   = htons(to.port);

            if (to.isV6) {
                // Native IPv6 target.
                memcpy(addr.sin6_addr.s6_addr, to.v6, 16);
            } else {
                // IPv4 target via IPv4-mapped address (::ffff:a.b.c.d).
                addr.sin6_addr.s6_addr[10] = 0xFF;
                addr.sin6_addr.s6_addr[11] = 0xFF;
                addr.sin6_addr.s6_addr[12] = to.ip[0];
                addr.sin6_addr.s6_addr[13] = to.ip[1];
                addr.sin6_addr.s6_addr[14] = to.ip[2];
                addr.sin6_addr.s6_addr[15] = to.ip[3];
            }
            sent = ::sendto(fd_, data, length, 0,
                            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        } else {
            // IPv4-only socket: ignore to.isV6, use ip[4].
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(to.port);
            addr.sin_addr.s_addr = htonl(
                (static_cast<uint32_t>(to.ip[0]) << 24) |
                (static_cast<uint32_t>(to.ip[1]) << 16) |
                (static_cast<uint32_t>(to.ip[2]) <<  8) |
                 static_cast<uint32_t>(to.ip[3]));
            sent = ::sendto(fd_, data, length, 0,
                            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        }
        return sent == static_cast<ssize_t>(length);
    }

    // Non-blocking: returns 0 when no datagram is waiting.
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override {
        if (fd_ < 0) return 0;

        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof(ss));
        socklen_t ssLen = sizeof(ss);

        ssize_t n = ::recvfrom(fd_, buffer, capacity, 0,
                               reinterpret_cast<struct sockaddr*>(&ss), &ssLen);
        if (n <= 0) return 0;

        fillEndpoint(from, &ss);
        return static_cast<size_t>(n);
    }

    // Returns the actual bound port.  Useful after begin(0) to discover the
    // ephemeral port the OS chose.
    uint16_t localPort() const {
        if (fd_ < 0) return 0;
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof(ss));
        socklen_t len = sizeof(ss);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0)
            return 0;
        if (ss.ss_family == AF_INET)
            return ntohs(reinterpret_cast<struct sockaddr_in*>(&ss)->sin_port);
        return ntohs(reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_port);
    }

private:
    // Translate a sockaddr_storage from recvfrom() into an Endpoint.
    // IPv4-mapped IPv6 addresses (::ffff:a.b.c.d) are demapped to plain IPv4.
    static void fillEndpoint(Endpoint& ep, const struct sockaddr_storage* ss) {
        ep = Endpoint{};
        if (ss->ss_family == AF_INET) {
            const struct sockaddr_in* a =
                reinterpret_cast<const struct sockaddr_in*>(ss);
            uint32_t raw = ntohl(a->sin_addr.s_addr);
            ep.ip[0] = static_cast<uint8_t>(raw >> 24);
            ep.ip[1] = static_cast<uint8_t>(raw >> 16);
            ep.ip[2] = static_cast<uint8_t>(raw >>  8);
            ep.ip[3] = static_cast<uint8_t>(raw);
            ep.port  = ntohs(a->sin_port);
            ep.isV6  = false;
        } else { // AF_INET6
            const struct sockaddr_in6* a =
                reinterpret_cast<const struct sockaddr_in6*>(ss);
            const uint8_t* b = a->sin6_addr.s6_addr;

            // Detect IPv4-mapped (::ffff:0:0/96): bytes 0-9 are 0, 10-11 are 0xFF.
            bool v4mapped =
                b[0]==0 && b[1]==0 && b[2]==0  && b[3]==0  &&
                b[4]==0 && b[5]==0 && b[6]==0  && b[7]==0  &&
                b[8]==0 && b[9]==0 && b[10]==0xFF && b[11]==0xFF;

            if (v4mapped) {
                ep.ip[0] = b[12]; ep.ip[1] = b[13];
                ep.ip[2] = b[14]; ep.ip[3] = b[15];
                ep.isV6  = false;
            } else {
                memcpy(ep.v6, b, 16);
                ep.isV6 = true;
            }
            ep.port = ntohs(a->sin6_port);
        }
    }

    int  fd_;
    bool dualStack_;
};

} // namespace pulsecoap

#endif // __unix__ || __linux__ || __APPLE__ || _POSIX_VERSION
