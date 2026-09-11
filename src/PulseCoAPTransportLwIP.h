// PulseCoAPTransportLwIP.h
// Transport adapter for bare-metal lwIP (2.x) raw UDP API.
//
// Usage
// -----
// 1. Build with PULSECOAP_TRANSPORT_LWIP=1, or simply include this header
//    after lwip/udp.h is on the include path (it auto-detects via LWIP_RAW).
// 2. Construct one LwIpUdpTransport, pass it to Server or Client.
// 3. In your main loop, call sys_check_timeouts() (and run your netif input
//    driver) BEFORE calling server.poll() / client.poll().
//
// IPv6
// ----
// When lwIP is built with LWIP_IPV6=1 this adapter uses udp_new_ip_type()
// to create a dual-stack PCB that accepts both IPv4 and IPv6 datagrams.
// Receive endpoints with isV6==true carry a 16-byte v6[] address; isV6==false
// carries a 4-byte ip[] IPv4 address — matching the Endpoint convention.
//
// Threading
// ---------
// This adapter assumes a cooperative single-threaded model (NO_SYS=1 or an
// RTOS where lwIP and the application run in the same task with interrupts
// only driving the netif Rx ISR that posts to a queue).  If your lwIP
// runs in a separate RTOS task, protect receive() and the internal ring
// with a mutex or use the lwIP netconn/sockets API instead.
//
// Ring size
// ---------
// PULSECOAP_LWIP_RX_SLOTS (default 4) sets how many incoming datagrams the
// ring can buffer between poll() calls.  If a burst arrives faster than
// poll() drains the ring, the oldest datagram is silently dropped (CoAP's
// own reliability layer will cause retransmission when needed).
#pragma once

#if defined(PULSECOAP_TRANSPORT_LWIP) || defined(LWIP_RAW)

#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "PulseCoAPConfig.h"
#include "PulseCoAPTransport.h"

namespace pulsecoap {

#ifndef PULSECOAP_LWIP_RX_SLOTS
#define PULSECOAP_LWIP_RX_SLOTS 4
#endif

class LwIpUdpTransport : public Transport {
public:
    LwIpUdpTransport() : pcb_(nullptr), head_(0), tail_(0) {
        memset(slots_, 0, sizeof(slots_));
    }

    ~LwIpUdpTransport() {
        if (pcb_) {
            udp_remove(pcb_);
            pcb_ = nullptr;
        }
    }

    bool begin(uint16_t localPort) override {
        if (pcb_) return false; // already started

#if LWIP_IPV6
        // Dual-stack PCB: accepts both IPv4 and IPv6.
        pcb_ = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (!pcb_) return false;
        {
            ip_addr_t any;
            IP_SET_TYPE_VAL(any, IPADDR_TYPE_ANY);
            ip_addr_set_any(1, &any); // in6addr_any equivalent
            if (udp_bind(pcb_, &any, localPort) != ERR_OK) {
                udp_remove(pcb_); pcb_ = nullptr;
                return false;
            }
        }
#else
        pcb_ = udp_new();
        if (!pcb_) return false;
        if (udp_bind(pcb_, IP_ADDR_ANY, localPort) != ERR_OK) {
            udp_remove(pcb_); pcb_ = nullptr;
            return false;
        }
#endif
        udp_recv(pcb_, recvCallback, this);
        return true;
    }

    bool send(const Endpoint& to, const uint8_t* data, size_t length) override {
        if (!pcb_ || length == 0) return false;

        struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT,
                                    static_cast<u16_t>(length), PBUF_RAM);
        if (!p) return false;
        memcpy(p->payload, data, length);

        ip_addr_t dest;
        err_t err;

#if LWIP_IPV6
        if (to.isV6) {
            IP_SET_TYPE_VAL(dest, IPADDR_TYPE_V6);
            // v6[] is MSB-first; lwIP ip6_addr stores 4 x uint32 in network order.
            memcpy(ip_2_ip6(&dest)->addr, to.v6, 16);
        } else {
            IP4_ADDR(&dest, to.ip[0], to.ip[1], to.ip[2], to.ip[3]);
        }
#else
        IP4_ADDR(&dest, to.ip[0], to.ip[1], to.ip[2], to.ip[3]);
#endif
        err = udp_sendto(pcb_, p, &dest, to.port);
        pbuf_free(p);
        return err == ERR_OK;
    }

    // Dequeues one datagram from the ring buffer. Returns 0 if none waiting.
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override {
        if (head_ == tail_) return 0;

        Slot& s = slots_[head_ % PULSECOAP_LWIP_RX_SLOTS];
        from = s.from;
        size_t toCopy = s.length < capacity ? s.length : capacity;
        memcpy(buffer, s.data, toCopy);
        ++head_;
        return toCopy;
    }

private:
    // ---------------------------------------------------------------------------
    // lwIP receive callback — called from sys_check_timeouts() / netif ISR.
    // Enqueues the datagram into the ring; drops if ring is full.
    // ---------------------------------------------------------------------------
    static void recvCallback(void* arg, struct udp_pcb* /*pcb*/,
                             struct pbuf* p, const ip_addr_t* addr, u16_t port) {
        if (!p) return;
        auto* self = static_cast<LwIpUdpTransport*>(arg);

        uint32_t used = self->tail_ - self->head_;
        if (used < PULSECOAP_LWIP_RX_SLOTS) {
            Slot& s = self->slots_[self->tail_ % PULSECOAP_LWIP_RX_SLOTS];

            // Copy payload — clamp to our max message size.
            s.length = p->tot_len < PULSECOAP_MAX_MSG_SIZE
                       ? p->tot_len : PULSECOAP_MAX_MSG_SIZE;
            pbuf_copy_partial(p, s.data, static_cast<u16_t>(s.length), 0);

            // Extract remote address.
            memset(&s.from, 0, sizeof(s.from));
            s.from.port = port;

#if LWIP_IPV6
            if (IP_IS_V6(addr)) {
                // Native IPv6 peer.
                const ip6_addr_t* ip6 = ip_2_ip6(addr);
                // lwIP stores each group of 4 bytes as a uint32 in network order.
                memcpy(s.from.v6, ip6->addr, 16);
                s.from.isV6 = true;
            } else
#endif
            {
                // IPv4 peer.
                const ip4_addr_t* ip4 = ip_2_ip4(addr);
                uint32_t raw = ip4_addr_get_u32(ip4); // host byte order
                s.from.ip[0] = static_cast<uint8_t>(raw        & 0xFF);
                s.from.ip[1] = static_cast<uint8_t>((raw >>  8) & 0xFF);
                s.from.ip[2] = static_cast<uint8_t>((raw >> 16) & 0xFF);
                s.from.ip[3] = static_cast<uint8_t>((raw >> 24) & 0xFF);
                s.from.isV6  = false;
            }

            ++self->tail_;
        }
        // Always free the pbuf — lwIP hands ownership of it to the callback.
        pbuf_free(p);
    }

    struct Slot {
        uint8_t  data[PULSECOAP_MAX_MSG_SIZE];
        size_t   length = 0;
        Endpoint from;
    };

    struct udp_pcb* pcb_;
    Slot    slots_[PULSECOAP_LWIP_RX_SLOTS];
    uint32_t head_; // consumer index (receive())
    uint32_t tail_; // producer index (recvCallback)
};

} // namespace pulsecoap

#endif // PULSECOAP_TRANSPORT_LWIP || LWIP_RAW
