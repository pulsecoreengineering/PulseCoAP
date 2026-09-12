// PulseCoAPGateway.h
// HTTP ↔ CoAP cross-protocol proxy (RFC 8075 style). POSIX-only.
//
// The Gateway bridges a web application (HTTP/1.1 over TCP) to CoAP devices
// (UDP). It owns a built-in non-blocking HTTP/1.1 listener and a PulseCoAP
// Client for the CoAP side.
//
// Request flow:
//   fetch() → TCP:5685 → Gateway → CoAP UDP → device → CoAP response
//   → HTTP response → web app
//
// Observe → Server-Sent Events:
//   EventSource → TCP:5685 → CoAP Observe → device notifications
//   → SSE data: events → EventSource callback
//
// Device registry:
//   A static table of { pathPrefix, Endpoint } entries. Requests are
//   dispatched to the device whose registered prefix is the longest match
//   of the HTTP path. The registry can be populated statically (addDevice)
//   or driven by multicast discovery (Client::discover()).
//
// Manual adapter:
//   If you already have an HTTP server (mongoose, libmicrohttpd, …) you can
//   skip begin()/poll() and call handleHttpRequest() directly from your HTTP
//   handler instead. That call blocks internally (spinning the CoAP loop)
//   until the device responds or PULSECOAP_GW_REQUEST_TIMEOUT_MS elapses.
//
// Build:
//   g++ -std=c++11 -Isrc your_app.cpp
//       src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp
//       src/PulseCoAPClient.cpp src/PulseCoAPGateway.cpp
#pragma once

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__) || \
    defined(_POSIX_VERSION)

#include "PulseCoAPConfig.h"

#if PULSECOAP_ENABLE_CLIENT

#include <stddef.h>
#include <stdint.h>

#include "PulseCoAPClient.h"
#include "PulseCoAPTransaction.h"
#include "PulseCoAPTransportPosix.h"
#include "PulseCoAPTypes.h"

namespace pulsecoap {

// ---------------------------------------------------------------------------
// Device registry entry
// ---------------------------------------------------------------------------
struct DeviceEntry {
    char     pathPrefix[PULSECOAP_MAX_URI_PATH_LEN] = {};
    Endpoint device;
    bool     active = false;
};

// ---------------------------------------------------------------------------
// Per-connection state (regular in-flight + SSE keep-alives share the pool)
// ---------------------------------------------------------------------------
struct HttpConn {
    bool     active          = false;
    bool     isSse           = false; // true → Observe subscription; keep-alive
    bool     headersComplete = false;
    bool     waitingCoap     = false; // CoAP request issued; awaiting callback
    int      fd              = -1;
    char     method[8]       = {};
    char     path[PULSECOAP_MAX_URI_PATH_LEN] = {};
    uint8_t  reqBuf[PULSECOAP_GW_MAX_HTTP_BODY + 768] = {}; // headers + body
    size_t   reqBufLen       = 0;
    int      contentLength   = 0;
    Endpoint coapDevice;    // device endpoint (stored for SSE cancel)
    uint32_t issuedAt        = 0; // nowMs when CoAP request was issued
};

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------
class Gateway {
public:
    Gateway();
    ~Gateway();

    // Add a path-prefix → device mapping. Returns false if the registry is full.
    // pathPrefix must start with '/' (e.g. "/sensors", "/actuators/led").
    // The longest matching prefix wins when dispatching a request.
    bool addDevice(const char* pathPrefix, const Endpoint& device);

    // Open the CoAP UDP socket (ephemeral port) and the HTTP TCP listener.
    // Call once before poll(). httpPort defaults to PULSECOAP_GW_HTTP_PORT.
    // Pass 0 to let the OS pick an ephemeral port (useful in tests).
    bool begin(uint16_t httpPort = PULSECOAP_GW_HTTP_PORT);

    // Open only the CoAP UDP socket (no HTTP listener). Use this when you
    // will call handleHttpRequest() from your own HTTP server.
    bool beginCoap();

    // Returns the TCP port the gateway's HTTP listener is bound to (0 if not
    // started or the bind failed).
    uint16_t httpPort() const;

    // Drive the gateway: accept HTTP connections, fill recv buffers, dispatch
    // completed requests, fire CoAP retransmits, flush SSE events.
    // Pass a monotonic millisecond counter (same source as your other poll() calls).
    void poll(uint32_t nowMs);

    // Manual adapter — call from your own HTTP server's request handler.
    // method:       "GET", "POST", "PUT", "DELETE", "OPTIONS"
    // path:         URI path (e.g. "/sensors/temp")
    // body/bodyLen: request body; nullptr/0 for GET/DELETE
    // outBuf:       caller-owned buffer to receive the response body
    // outLen:       bytes written into outBuf on return
    // isSse:        if non-null and set to true on return, the caller must keep
    //               the connection open for SSE; call registerSseConn(fd, path)
    //               and include the fd in subsequent poll() drives
    // Returns the HTTP status code (200, 201, 204, 404, 504, …).
    int handleHttpRequest(const char* method, const char* path,
                           const uint8_t* body, size_t bodyLen,
                           uint8_t* outBuf, size_t outCapacity, size_t& outLen,
                           bool* isSse = nullptr);

private:
    bool   findDevice(const char* path, Endpoint& out) const;
    int    allocConn();
    void   closeConn(int i);
    bool   parseHeaders(int i);
    void   dispatchConn(int i, uint32_t nowMs);
    void   writeResponse(int fd, int status, const char* contentType,
                          const uint8_t* body, size_t bodyLen);
    void   writeSseHeaders(int fd);
    void   writeSseEvent(int fd, const uint8_t* data, size_t len);

    static int  coapCodeToHttp(Code c, size_t payloadLen);
    static Code httpMethodToCoap(const char* method);

    static void onCoapResponse(const ClientResponse& res, void* ctx);
    static void onCoapTimeout(void* ctx);

    PosixUdpTransport transport_;
    TransactionPool   transactions_;
    Client            client_;
    int               listenFd_   = -1;
    uint32_t          nowMs_      = 0;

    DeviceEntry devices_[PULSECOAP_GW_MAX_DEVICES];
    HttpConn    conns_[PULSECOAP_GW_MAX_SSE_SESSIONS];
};

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_CLIENT
#endif // POSIX
