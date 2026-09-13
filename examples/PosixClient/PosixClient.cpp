// PosixClient.cpp
// Minimal CoAP client for Linux / macOS / Raspberry Pi (any POSIX host).
// No Arduino toolchain, no device — builds with a plain g++/clang++.
//
// Build:
//   g++ -std=c++11 -Isrc \
//       examples/PosixClient/PosixClient.cpp \
//       src/PulseCoAPMessage.cpp \
//       src/PulseCoAPTransaction.cpp \
//       src/PulseCoAPServer.cpp \
//       src/PulseCoAPClient.cpp \
//       -o coap_client
//
// Usage:
//   ./coap_client coap.me /hello
//   ./coap_client californium.eclipseprojects.io /test
//   ./coap_client 192.168.1.42 /sensors/temp

#include <stdio.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include "PulseCoAP.h"
#include "PulseCoAPTransportPosix.h"

using namespace pulsecoap;

static uint32_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}

static bool done = false;

static void onResponse(const ClientResponse& res, void*) {
    printf("  code:    %d.%02d\n", (int)res.code >> 5, (int)res.code & 0x1f);
    if (res.payloadLength > 0)
        printf("  payload: %.*s\n", (int)res.payloadLength, (char*)res.payload);
    done = true;
}

static void onTimeout(void*) {
    printf("  timeout — no response within 15 s\n");
    done = true;
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "coap.me";
    const char* path = argc > 2 ? argv[2] : "/hello";

    // Resolve hostname to an IPv4 address
    struct addrinfo hints = {}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, "5683", &hints, &res) != 0) {
        fprintf(stderr, "error: DNS lookup failed for '%s'\n", host);
        return 1;
    }
    Endpoint ep = {};
    uint32_t raw = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    ep.ip[0] = (raw >>  0) & 0xff;
    ep.ip[1] = (raw >>  8) & 0xff;
    ep.ip[2] = (raw >> 16) & 0xff;
    ep.ip[3] = (raw >> 24) & 0xff;
    ep.port  = 5683;
    freeaddrinfo(res);

    printf("GET coap://%s:5683%s\n", host, path);

    PosixUdpTransport transport;
    TransactionPool   txPool;
    Client            client(transport, txPool);
    client.setTimeoutHandler(onTimeout);

    if (!client.begin()) {
        fprintf(stderr, "error: failed to open UDP socket\n");
        return 1;
    }

    client.get(ep, path, onResponse);

    // Spin the CoAP loop until we get a response or time out
    uint32_t start = nowMs();
    while (!done && nowMs() - start < 15000) {
        client.poll(nowMs());
        usleep(5000);
    }

    return 0;
}
