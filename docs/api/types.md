# Types & Constants

```cpp
#include <PulseCoAP.h>
using namespace pulsecoap;
```

---

## Endpoint

```cpp
struct Endpoint {
    uint8_t  ip[4]  = {0, 0, 0, 0};  // IPv4 address, MSB first
    uint8_t  v6[16] = {};             // IPv6 address, MSB first (RFC 4291)
    bool     isV6   = false;
    uint16_t port   = 0;
};
```

Identifies a UDP remote address + port.

When `isV6 == false` (the default), the IPv4 address is in `ip[4]`. All existing code that uses `ep.ip[0]…ip[3]` works unchanged.

When `isV6 == true`, the IPv6 address is in `v6[16]` and `ip[4]` is ignored. Transport adapters that open a dual-stack socket automatically demap incoming IPv4-mapped addresses (`::ffff:a.b.c.d`) — callers always see plain IPv4 for IPv4 peers.

**Common usage:**

```cpp
// IPv4 device at 192.168.1.42, standard CoAP port
pulsecoap::Endpoint sensor = {{192, 168, 1, 42}, 5683};

// IPv6
pulsecoap::Endpoint v6sensor;
v6sensor.isV6 = true;
v6sensor.port = 5683;
memcpy(v6sensor.v6, myIpv6Address, 16);
```

`Endpoint` supports `==` and `!=` comparison.

---

## Code

CoAP method and response codes (RFC 7252 §12.1).

```cpp
enum class Code : uint8_t {
    Empty   = 0x00,

    // Methods (0.xx)
    Get    = 0x01,
    Post   = 0x02,
    Put    = 0x03,
    Delete = 0x04,
    Fetch  = 0x05,   // RFC 8132
    Patch  = 0x06,   // RFC 8132
    IPatch = 0x07,   // RFC 8132

    // 2.xx Success
    Created  = 0x41,   // 2.01
    Deleted  = 0x42,   // 2.02
    Valid    = 0x43,   // 2.03
    Changed  = 0x44,   // 2.04
    Content  = 0x45,   // 2.05 — most common GET response
    Continue = 0x5F,   // 2.31 — block-wise continue

    // 4.xx Client error
    BadRequest              = 0x80,   // 4.00
    Unauthorized            = 0x81,   // 4.01
    BadOption               = 0x82,   // 4.02
    Forbidden               = 0x83,   // 4.03
    NotFound                = 0x84,   // 4.04
    MethodNotAllowed        = 0x85,   // 4.05
    NotAcceptable           = 0x86,   // 4.06
    RequestEntityIncomplete = 0x88,   // 4.08
    PreconditionFailed      = 0x8C,   // 4.12
    RequestEntityTooLarge   = 0x8D,   // 4.13
    UnsupportedContentFormat = 0x8F,  // 4.15

    // 5.xx Server error
    InternalServerError  = 0xA0,   // 5.00
    NotImplemented       = 0xA1,   // 5.01
    BadGateway           = 0xA2,   // 5.02
    ServiceUnavailable   = 0xA3,   // 5.03
    GatewayTimeout       = 0xA4,   // 5.04
    ProxyingNotSupported = 0xA5,   // 5.05
};
```

The numeric value encodes class and detail: `(class << 5) | detail`. The traditional dotted notation (e.g. "2.05") maps directly.

Helper functions:

```cpp
uint8_t codeClass(uint8_t code);    // returns 0, 2, 4, or 5
uint8_t codeDetail(uint8_t code);   // returns the detail nibble
bool isMethodCode(Code code);       // true for Get/Post/Put/Delete/Fetch/Patch/IPatch
```

**Setting a response code in a handler:**

```cpp
void handleTemp(const Request& /*req*/, Response& res, void*) {
    res.code = Code::Content;           // 2.05
    res.setPayload("24.5");
}

void handleUnknown(const Request& /*req*/, Response& res, void*) {
    res.code = Code::NotFound;          // 4.04
    res.setPayload("resource not found");
}
```

---

## ContentFormat

IANA Content-Format identifiers (RFC 7252 §12.3):

```cpp
enum class ContentFormat : uint16_t {
    TextPlain   = 0,    // text/plain;charset=utf-8
    LinkFormat  = 40,   // application/link-format (CoRE, RFC 6690)
    Xml         = 41,   // application/xml
    OctetStream = 42,   // application/octet-stream
    Exi         = 47,   // application/exi
    Json        = 50,   // application/json
    Cbor        = 60,   // application/cbor (RFC 7049)
};
```

`TextPlain` is the default for `Response` and `notify()`.

```cpp
// JSON payload:
res.contentFormat = ContentFormat::Json;
res.setPayload("{\"temp\":24.5}");
```

---

## MessageType

```cpp
enum class MessageType : uint8_t {
    Confirmable     = 0,   // CON — retransmitted until ACK'd
    NonConfirmable  = 1,   // NON — fire-and-forget
    Acknowledgement = 2,   // ACK — confirms a CON
    Reset           = 3,   // RST — message could not be processed
};
```

You rarely need this directly — `Server` and `Client` handle message types internally. It is exposed on `Message` for advanced use (e.g. inspecting `req.message->type()` in a handler).

---

## OptionNumber

Selected option numbers from RFC 7252 §12.2, RFC 7641, and RFC 7959:

| Constant | Value | Meaning |
|---|---|---|
| `OptionNumber::IfMatch` | 1 | ETag match condition |
| `OptionNumber::UriHost` | 3 | Request host |
| `OptionNumber::ETag` | 4 | Entity tag |
| `OptionNumber::Observe` | 6 | RFC 7641 subscribe/notify |
| `OptionNumber::UriPort` | 7 | Request port |
| `OptionNumber::UriPath` | 11 | Path segment (one option per segment) |
| `OptionNumber::ContentFormat` | 12 | Payload content format |
| `OptionNumber::MaxAge` | 14 | Max-Age for caching |
| `OptionNumber::UriQuery` | 15 | Query parameter |
| `OptionNumber::Block2` | 23 | RFC 7959 block-wise response |
| `OptionNumber::Block1` | 27 | RFC 7959 block-wise request |

Access options from inside a handler via `req.message->option(OptionNumber::...)`.

---

## Observe constants

```cpp
constexpr uint32_t kObserveRegister   = 0;   // Observe option value to subscribe
constexpr uint32_t kObserveDeregister = 1;   // Observe option value to unsubscribe
```

Set automatically by `Client::observe()` and `Client::cancelObserve()`. Exposed for advanced use (e.g. reading the Observe value from `req.message` in a handler).
