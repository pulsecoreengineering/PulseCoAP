#include "PulseCoAPGateway.h"

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__) || \
    defined(_POSIX_VERSION)

#if PULSECOAP_ENABLE_CLIENT

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace pulsecoap {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static uint32_t monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// Write exactly `len` bytes to a non-blocking fd; ignore partial sends
// (the TCP buffer is always large enough for our small HTTP responses).
static void writeAll(int fd, const void* data, size_t len) {
    if (fd < 0 || !data || len == 0) return;
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::write(fd, p, len);
        if (n <= 0) return;
        p   += n;
        len -= static_cast<size_t>(n);
    }
}

// Parse a decimal integer from a NUL-terminated string; returns -1 on error.
static int parseDecimal(const char* s) {
    if (!s || *s == '\0') return -1;
    int v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

// Case-insensitive strstr (headers arrive with varied case).
static const char* ciStrStr(const char* hay, const char* needle) {
    size_t nlen = strlen(needle);
    for (; *hay; ++hay) {
        if (strncasecmp(hay, needle, nlen) == 0) return hay;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// CoAP response/timeout callbacks
// ---------------------------------------------------------------------------

// userContext = pointer to the HttpConn that issued the CoAP request.
// For SSE connects the same callback fires for every notification.
void Gateway::onCoapResponse(const ClientResponse& res, void* ctx) {
    HttpConn* c = static_cast<HttpConn*>(ctx);
    if (!c || !c->active || c->fd < 0) return;

    if (c->isSse) {
        // Stream the payload as one SSE event.
        c->waitingCoap = false; // first response received; keep-alive from here
        // Write SSE event directly: `data: <payload>\n\n`
        const char prefix[] = "data: ";
        writeAll(c->fd, prefix, sizeof(prefix) - 1);
        if (res.payload && res.payloadLength > 0)
            writeAll(c->fd, res.payload, res.payloadLength);
        writeAll(c->fd, "\n\n", 2);
    } else {
        int status = coapCodeToHttp(res.code, res.payloadLength);
        const char* ct = "text/plain";
        // Infer Content-Type from CoAP Content-Format
        if (res.contentFormat == ContentFormat::Json)        ct = "application/json";
        else if (res.contentFormat == ContentFormat::Xml)    ct = "application/xml";
        else if (res.contentFormat == ContentFormat::LinkFormat) ct = "application/link-format";
        else if (res.contentFormat == ContentFormat::OctetStream) ct = "application/octet-stream";

        // Build response into a stack buffer (Gateway's rxBuffer is ~PULSECOAP_MAX_MSG_SIZE).
        char hdr[256];
        const char* phrase = "OK";
        if      (status == 201) phrase = "Created";
        else if (status == 204) phrase = "No Content";
        else if (status == 400) phrase = "Bad Request";
        else if (status == 401) phrase = "Unauthorized";
        else if (status == 403) phrase = "Forbidden";
        else if (status == 404) phrase = "Not Found";
        else if (status == 405) phrase = "Method Not Allowed";
        else if (status == 413) phrase = "Payload Too Large";
        else if (status == 415) phrase = "Unsupported Media Type";
        else if (status == 500) phrase = "Internal Server Error";
        else if (status == 503) phrase = "Service Unavailable";
        else if (status == 504) phrase = "Gateway Timeout";

        int hdrLen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            status, phrase, ct,
            (status == 204) ? (size_t)0 : res.payloadLength);
        if (hdrLen > 0) writeAll(c->fd, hdr, static_cast<size_t>(hdrLen));
        if (status != 204 && res.payload && res.payloadLength > 0)
            writeAll(c->fd, res.payload, res.payloadLength);

        ::close(c->fd);
        c->fd     = -1;
        c->active = false;
    }
}

void Gateway::onCoapTimeout(void* ctx) {
    HttpConn* c = static_cast<HttpConn*>(ctx);
    if (!c || !c->active || c->fd < 0) return;

    const char body[] = "CoAP device did not respond";
    char hdr[256];
    int hdrLen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 504 Gateway Timeout\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        sizeof(body) - 1);
    if (hdrLen > 0) writeAll(c->fd, hdr, static_cast<size_t>(hdrLen));
    writeAll(c->fd, body, sizeof(body) - 1);
    ::close(c->fd);
    c->fd     = -1;
    c->active = false;
}

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

Gateway::Gateway() : client_(transport_, transactions_) {}

Gateway::~Gateway() {
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    for (int i = 0; i < PULSECOAP_GW_MAX_SSE_SESSIONS; ++i) {
        if (conns_[i].fd >= 0) { ::close(conns_[i].fd); conns_[i].fd = -1; }
    }
}

bool Gateway::addDevice(const char* pathPrefix, const Endpoint& device) {
    for (int i = 0; i < PULSECOAP_GW_MAX_DEVICES; ++i) {
        if (!devices_[i].active) {
            size_t len = strlen(pathPrefix);
            if (len >= PULSECOAP_MAX_URI_PATH_LEN) return false;
            memcpy(devices_[i].pathPrefix, pathPrefix, len + 1);
            devices_[i].device = device;
            devices_[i].active = true;
            return true;
        }
    }
    return false; // table full
}

bool Gateway::findDevice(const char* path, Endpoint& out) const {
    int   bestLen = -1;
    const DeviceEntry* bestEntry = nullptr;
    for (int i = 0; i < PULSECOAP_GW_MAX_DEVICES; ++i) {
        if (!devices_[i].active) continue;
        const char* prefix = devices_[i].pathPrefix;
        size_t plen = strlen(prefix);
        if (strncmp(path, prefix, plen) == 0 &&
            (path[plen] == '\0' || path[plen] == '/')) {
            if (static_cast<int>(plen) > bestLen) {
                bestLen   = static_cast<int>(plen);
                bestEntry = &devices_[i];
            }
        }
    }
    if (!bestEntry) return false;
    out = bestEntry->device;
    return true;
}

int Gateway::allocConn() {
    for (int i = 0; i < PULSECOAP_GW_MAX_SSE_SESSIONS; ++i) {
        if (!conns_[i].active) {
            conns_[i] = HttpConn{};
            conns_[i].active = true;
            return i;
        }
    }
    return -1;
}

void Gateway::closeConn(int i) {
    if (conns_[i].fd >= 0) { ::close(conns_[i].fd); conns_[i].fd = -1; }
    conns_[i].active = false;
}

bool Gateway::beginCoap() {
    if (!transport_.begin(0)) return false;
    client_.setTimeoutHandler(onCoapTimeout);
    return true;
}

uint16_t Gateway::httpPort() const {
    if (listenFd_ < 0) return 0;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

bool Gateway::begin(uint16_t httpPort) {
    if (!transport_.begin(0)) return false;
    client_.setTimeoutHandler(onCoapTimeout);

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // Non-blocking accept
    int flags = ::fcntl(listenFd_, F_GETFL, 0);
    if (flags >= 0) ::fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(httpPort);
    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        ::close(listenFd_); listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 8) < 0) {
        ::close(listenFd_); listenFd_ = -1;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// HTTP header parser — fills conn.method, conn.path, conn.contentLength,
// conn.isSse. Returns true when headers are complete; false to wait for more.
// ---------------------------------------------------------------------------
bool Gateway::parseHeaders(int i) {
    HttpConn& c = conns_[i];
    const char* buf = reinterpret_cast<char*>(c.reqBuf);
    size_t len = c.reqBufLen;

    // Look for end-of-headers
    const char* hdrEnd = strstr(buf, "\r\n\r\n");
    if (!hdrEnd) return false;

    // Parse request line: METHOD /path HTTP/1.1
    const char* lineEnd = strstr(buf, "\r\n");
    if (!lineEnd) return false;

    // method
    const char* p = buf;
    size_t mlen = 0;
    while (*p && *p != ' ' && mlen < sizeof(c.method) - 1)
        c.method[mlen++] = *p++;
    c.method[mlen] = '\0';
    if (*p == ' ') ++p;

    // path (up to next space or \r)
    size_t plen = 0;
    while (*p && *p != ' ' && *p != '\r' && plen < sizeof(c.path) - 1)
        c.path[plen++] = *p++;
    c.path[plen] = '\0';

    // Scan headers between request line and \r\n\r\n
    const char* h = lineEnd + 2; // skip first \r\n
    while (h < hdrEnd) {
        const char* hLineEnd = strstr(h, "\r\n");
        if (!hLineEnd || hLineEnd > hdrEnd) break;

        if (ciStrStr(h, "content-length:")) {
            const char* v = strchr(h, ':');
            if (v) { while (*++v == ' ') {} c.contentLength = parseDecimal(v); }
        } else if (ciStrStr(h, "accept:")) {
            if (ciStrStr(h, "text/event-stream")) c.isSse = true;
        }
        h = hLineEnd + 2;
    }

    c.headersComplete = true;
    // Move body bytes (if any already received) to front of buffer.
    const char* bodyStart = hdrEnd + 4;
    size_t bodyAlready = static_cast<size_t>(buf + len - bodyStart);
    if (bodyAlready > 0)
        memmove(c.reqBuf, bodyStart, bodyAlready);
    c.reqBufLen = bodyAlready;
    return true;
}

// ---------------------------------------------------------------------------
// dispatchConn: called when the request is fully received (headers + body).
// Issues the CoAP request or responds immediately for OPTIONS / bad paths.
// ---------------------------------------------------------------------------
void Gateway::dispatchConn(int i, uint32_t nowMs) {
    HttpConn& c = conns_[i];
    const char* path = c.path;
    const char* method = c.method;

    // CORS preflight
    if (strcmp(method, "OPTIONS") == 0) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n");
        writeAll(c.fd, hdr, strlen(hdr));
        closeConn(i);
        return;
    }

    Endpoint device;
    if (!findDevice(path, device)) {
        const char body[] = "No device registered for path";
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 404 Not Found\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            sizeof(body) - 1);
        writeAll(c.fd, hdr, strlen(hdr));
        writeAll(c.fd, body, sizeof(body) - 1);
        closeConn(i);
        return;
    }

    c.coapDevice  = device;
    c.issuedAt    = nowMs;
    c.waitingCoap = true;

    Code coapMethod = httpMethodToCoap(method);

    if (c.isSse) {
        // Send SSE headers immediately; keep socket open.
        writeSseHeaders(c.fd);
        client_.observe(device, path, onCoapResponse, &c);
    } else {
        bool ok = false;
        if (coapMethod == Code::Get || coapMethod == Code::Delete) {
            ok = (coapMethod == Code::Get)
                ? client_.get(device, path, onCoapResponse, &c)
                : client_.del(device, path, onCoapResponse, &c);
        } else {
            // POST or PUT — forward the body
            const uint8_t* body = c.reqBuf;
            size_t bodyLen = static_cast<size_t>(c.contentLength > 0 ? c.contentLength : 0);
            if (bodyLen > c.reqBufLen) bodyLen = c.reqBufLen;
            ok = (coapMethod == Code::Post)
                ? client_.post(device, path, body, bodyLen,
                               ContentFormat::Json, onCoapResponse, &c)
                : client_.put(device, path, body, bodyLen,
                              ContentFormat::Json, onCoapResponse, &c);
        }
        if (!ok) {
            // Failed to issue CoAP (pool full, encode error, send failure).
            const char body[] = "CoAP dispatch failed";
            char hdr[256];
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n",
                sizeof(body) - 1);
            writeAll(c.fd, hdr, strlen(hdr));
            writeAll(c.fd, body, sizeof(body) - 1);
            closeConn(i);
        }
    }
}

void Gateway::writeSseHeaders(int fd) {
    const char hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    writeAll(fd, hdr, sizeof(hdr) - 1);
}

void Gateway::writeSseEvent(int fd, const uint8_t* data, size_t len) {
    writeAll(fd, "data: ", 6);
    if (data && len) writeAll(fd, data, len);
    writeAll(fd, "\n\n", 2);
}

void Gateway::writeResponse(int fd, int status, const char* contentType,
                             const uint8_t* body, size_t bodyLen) {
    const char* phrase = "OK";
    if      (status == 201) phrase = "Created";
    else if (status == 204) phrase = "No Content";
    else if (status == 404) phrase = "Not Found";
    else if (status == 503) phrase = "Service Unavailable";
    else if (status == 504) phrase = "Gateway Timeout";

    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, phrase, contentType, bodyLen);
    if (n > 0) writeAll(fd, hdr, static_cast<size_t>(n));
    if (body && bodyLen) writeAll(fd, body, bodyLen);
}

// ---------------------------------------------------------------------------
// poll()
// ---------------------------------------------------------------------------
void Gateway::poll(uint32_t nowMs) {
    nowMs_ = nowMs;

    // 1. Accept new TCP connections.
    if (listenFd_ >= 0) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd >= 0) {
            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int slot = allocConn();
            if (slot >= 0) {
                conns_[slot].fd = fd;
            } else {
                // No room — reject immediately.
                const char msg[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
                writeAll(fd, msg, sizeof(msg) - 1);
                ::close(fd);
            }
        }
    }

    // 2. Read from active connections.
    for (int i = 0; i < PULSECOAP_GW_MAX_SSE_SESSIONS; ++i) {
        HttpConn& c = conns_[i];
        if (!c.active || c.fd < 0) continue;

        if (!c.headersComplete) {
            // Accumulate into reqBuf.
            size_t cap = sizeof(c.reqBuf) - c.reqBufLen - 1;
            if (cap > 0) {
                ssize_t n = ::recv(c.fd, c.reqBuf + c.reqBufLen, cap, 0);
                if (n > 0) {
                    c.reqBufLen += static_cast<size_t>(n);
                    c.reqBuf[c.reqBufLen] = '\0'; // for strstr
                } else if (n == 0) {
                    closeConn(i); continue; // peer closed
                }
                // n < 0 with EAGAIN/EWOULDBLOCK → nothing to read yet, that's OK
            }
            if (c.reqBufLen > 0 && !c.headersComplete)
                parseHeaders(i);
        }

        // If headers complete and body fully received, dispatch.
        if (c.headersComplete && !c.waitingCoap && !c.isSse) {
            int needed = c.contentLength > 0 ? c.contentLength : 0;
            if (static_cast<int>(c.reqBufLen) >= needed) {
                dispatchConn(i, nowMs);
            }
        } else if (c.headersComplete && !c.waitingCoap && c.isSse) {
            // isSse was set during parseHeaders but not dispatched yet.
            dispatchConn(i, nowMs);
        }

        // Timeout in-flight (non-SSE) requests.
        if (c.active && c.waitingCoap && !c.isSse) {
            if ((nowMs - c.issuedAt) >= PULSECOAP_GW_REQUEST_TIMEOUT_MS) {
                onCoapTimeout(&c);
            }
        }

        // Detect broken SSE connections by attempting a zero-byte write.
        if (c.active && c.isSse && !c.waitingCoap && c.fd >= 0) {
            ssize_t probe = ::send(c.fd, "", 0, MSG_NOSIGNAL);
            if (probe < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // SSE client disconnected; cancel the Observe.
                client_.cancelObserve(c.coapDevice, c.path);
                closeConn(i);
            }
        }
    }

    // 3. Drive CoAP.
    client_.poll(nowMs);
}

// ---------------------------------------------------------------------------
// handleHttpRequest — synchronous adapter for external HTTP servers
// ---------------------------------------------------------------------------
int Gateway::handleHttpRequest(const char* method, const char* path,
                                const uint8_t* body, size_t bodyLen,
                                uint8_t* outBuf, size_t outCapacity, size_t& outLen,
                                bool* isSse) {
    outLen = 0;
    if (isSse) *isSse = false;

    // OPTIONS preflight
    if (strcmp(method, "OPTIONS") == 0) return 204;

    Endpoint device;
    if (!findDevice(path, device)) return 404;

    if (isSse && ciStrStr("text/event-stream", "event-stream")) {
        // Caller handles SSE keep-alive; we just register the observe.
        if (isSse) *isSse = true;
        return 200;
    }

    // Synchronous: issue CoAP request and spin the loop until done or timeout.
    struct Awaiter {
        bool         done = false;
        int          httpStatus = 504;
        const uint8_t* payload = nullptr;
        size_t         payloadLen = 0;
        ContentFormat  cf = ContentFormat::TextPlain;
    } awaiter;

    auto cb = [](const ClientResponse& res, void* ctx) {
        Awaiter* a = static_cast<Awaiter*>(ctx);
        a->done       = true;
        a->httpStatus = Gateway::coapCodeToHttp(res.code, res.payloadLength);
        a->payload    = res.payload;
        a->payloadLen = res.payloadLength;
        a->cf         = res.contentFormat;
    };

    Code coapMethod = httpMethodToCoap(method);
    bool ok = false;
    if (coapMethod == Code::Get) {
        ok = client_.get(device, path, cb, &awaiter);
    } else if (coapMethod == Code::Delete) {
        ok = client_.del(device, path, cb, &awaiter);
    } else if (coapMethod == Code::Post) {
        ok = client_.post(device, path, body, bodyLen,
                          ContentFormat::Json, cb, &awaiter);
    } else {
        ok = client_.put(device, path, body, bodyLen,
                         ContentFormat::Json, cb, &awaiter);
    }
    if (!ok) return 503;

    uint32_t start = monotonicMs();
    while (!awaiter.done) {
        uint32_t elapsed = monotonicMs() - start;
        if (elapsed >= PULSECOAP_GW_REQUEST_TIMEOUT_MS) break;
        client_.poll(elapsed);
        struct timespec ts = {0, 1000000}; // 1 ms
        nanosleep(&ts, nullptr);
    }
    if (!awaiter.done) return 504;

    if (awaiter.payload && awaiter.payloadLen > 0) {
        outLen = awaiter.payloadLen < outCapacity ? awaiter.payloadLen : outCapacity;
        memcpy(outBuf, awaiter.payload, outLen);
    }
    return awaiter.httpStatus;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

int Gateway::coapCodeToHttp(Code c, size_t payloadLen) {
    switch (c) {
    case Code::Created:  return 201;
    case Code::Deleted:  return 204;
    case Code::Valid:    return 304;
    case Code::Changed:  return payloadLen > 0 ? 200 : 204;
    case Code::Content:  return 200;
    case Code::BadRequest:              return 400;
    case Code::Unauthorized:            return 401;
    case Code::Forbidden:               return 403;
    case Code::NotFound:                return 404;
    case Code::MethodNotAllowed:        return 405;
    case Code::NotAcceptable:           return 406;
    case Code::PreconditionFailed:      return 412;
    case Code::RequestEntityTooLarge:   return 413;
    case Code::UnsupportedContentFormat: return 415;
    case Code::InternalServerError:     return 500;
    case Code::NotImplemented:          return 501;
    case Code::BadGateway:              return 502;
    case Code::ServiceUnavailable:      return 503;
    case Code::GatewayTimeout:          return 504;
    default:                            return 500;
    }
}

Code Gateway::httpMethodToCoap(const char* method) {
    if (strcmp(method, "POST")   == 0) return Code::Post;
    if (strcmp(method, "PUT")    == 0) return Code::Put;
    if (strcmp(method, "DELETE") == 0) return Code::Delete;
    return Code::Get; // GET + unknown → GET
}

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_CLIENT
#endif // POSIX
