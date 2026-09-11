// Test-only Transport double: an in-process "network" connecting exactly
// two Transport instances, so client/server integration tests can run on
// a host with no real sockets. Not part of the shipped library.
#pragma once

#include <cstring>
#include <deque>
#include <vector>

#include "../src/PulseCoAPTransport.h"

class LoopbackTransport : public pulsecoap::Transport {
public:
    void connectPeer(LoopbackTransport* peer) { peer_ = peer; }

    bool begin(uint16_t localPort) override {
        localPort_ = localPort;
        return true;
    }

    bool send(const pulsecoap::Endpoint& to, const uint8_t* data, size_t length) override {
        (void)to; // single fixed peer in this test double
        if (!peer_) return false;
        Datagram dg;
        dg.from.ip[0] = 127; dg.from.ip[1] = 0; dg.from.ip[2] = 0; dg.from.ip[3] = 1;
        dg.from.port = localPort_;
        dg.data.assign(data, data + length);
        peer_->inbox_.push_back(dg);
        return true;
    }

    size_t receive(uint8_t* buffer, size_t capacity, pulsecoap::Endpoint& from) override {
        if (inbox_.empty()) return 0;
        Datagram dg = inbox_.front();
        inbox_.pop_front();
        size_t n = dg.data.size() < capacity ? dg.data.size() : capacity;
        std::memcpy(buffer, dg.data.data(), n);
        from = dg.from;
        return n;
    }

    bool hasPending() const { return !inbox_.empty(); }

    // Directly place a raw datagram into this transport's inbox — used by
    // tests to inject crafted frames (e.g. RST) without going through a peer.
    void injectPacket(const uint8_t* data, size_t length,
                      const pulsecoap::Endpoint& from) {
        Datagram dg;
        dg.from = from;
        dg.data.assign(data, data + length);
        inbox_.push_back(dg);
    }

    // Non-destructive peek at the front datagram — returns its length and
    // fills buffer/from without removing it from the inbox.
    size_t peek(uint8_t* buffer, size_t capacity,
                pulsecoap::Endpoint& from) const {
        if (inbox_.empty()) return 0;
        const Datagram& dg = inbox_.front();
        size_t n = dg.data.size() < capacity ? dg.data.size() : capacity;
        std::memcpy(buffer, dg.data.data(), n);
        from = dg.from;
        return n;
    }

private:
    struct Datagram {
        pulsecoap::Endpoint from;
        std::vector<uint8_t> data;
    };

    uint16_t localPort_ = 0;
    LoopbackTransport* peer_ = nullptr;
    std::deque<Datagram> inbox_;
};
