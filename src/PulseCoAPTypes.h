// PulseCoAPTypes.h
// Wire-format constants from RFC 7252 (CoAP) and RFC 7959 (Block-wise) /
// RFC 7641 (Observe). Plain enums/constants, no allocation, no dependencies.
#pragma once

#include <stdint.h>

namespace pulsecoap {

// CoAP version this library speaks (RFC 7252 §3: always 1).
constexpr uint8_t kVersion = 1;

// --- Message type (RFC 7252 §3) --------------------------------------------
enum class MessageType : uint8_t {
    Confirmable    = 0, // CON
    NonConfirmable = 1, // NON
    Acknowledgement = 2, // ACK
    Reset          = 3  // RST
};

// --- Code (RFC 7252 §3, §12.1): (class << 5) | detail -----------------------
constexpr uint8_t makeCode(uint8_t cls, uint8_t detail) {
    return static_cast<uint8_t>(((cls & 0x07) << 5) | (detail & 0x1F));
}
constexpr uint8_t codeClass(uint8_t code)  { return static_cast<uint8_t>(code >> 5); }
constexpr uint8_t codeDetail(uint8_t code) { return static_cast<uint8_t>(code & 0x1F); }

enum class Code : uint8_t {
    Empty  = makeCode(0, 0),

    // Method codes, 0.xx (RFC 7252 §12.1.1, RFC 8132 for FETCH/PATCH/iPATCH)
    Get    = makeCode(0, 1),
    Post   = makeCode(0, 2),
    Put    = makeCode(0, 3),
    Delete = makeCode(0, 4),
    Fetch  = makeCode(0, 5),
    Patch  = makeCode(0, 6),
    IPatch = makeCode(0, 7),

    // Success, 2.xx
    Created  = makeCode(2, 1),
    Deleted  = makeCode(2, 2),
    Valid    = makeCode(2, 3),
    Changed  = makeCode(2, 4),
    Content  = makeCode(2, 5),
    Continue = makeCode(2, 31),

    // Client error, 4.xx
    BadRequest              = makeCode(4, 0),
    Unauthorized            = makeCode(4, 1),
    BadOption               = makeCode(4, 2),
    Forbidden               = makeCode(4, 3),
    NotFound                = makeCode(4, 4),
    MethodNotAllowed        = makeCode(4, 5),
    NotAcceptable           = makeCode(4, 6),
    RequestEntityIncomplete = makeCode(4, 8),
    PreconditionFailed      = makeCode(4, 12),
    RequestEntityTooLarge   = makeCode(4, 13),
    UnsupportedContentFormat = makeCode(4, 15),

    // Server error, 5.xx
    InternalServerError = makeCode(5, 0),
    NotImplemented       = makeCode(5, 1),
    BadGateway           = makeCode(5, 2),
    ServiceUnavailable   = makeCode(5, 3),
    GatewayTimeout       = makeCode(5, 4),
    ProxyingNotSupported = makeCode(5, 5)
};

inline bool isMethodCode(Code code) { return codeClass(static_cast<uint8_t>(code)) == 0 && static_cast<uint8_t>(code) != 0; }

// --- Option numbers (RFC 7252 §12.2, RFC 7641 §2, RFC 7959 §2) -------------
enum class OptionNumber : uint16_t {
    IfMatch       = 1,
    UriHost       = 3,
    ETag          = 4,
    IfNoneMatch   = 5,
    Observe       = 6,   // RFC 7641
    UriPort       = 7,
    LocationPath  = 8,
    UriPath       = 11,
    ContentFormat = 12,
    MaxAge        = 14,
    UriQuery      = 15,
    Accept        = 17,
    LocationQuery = 20,
    Block2        = 23,  // RFC 7959
    Block1        = 27,  // RFC 7959
    Size2         = 28,  // RFC 7959
    ProxyUri      = 35,
    ProxyScheme   = 39,
    Size1         = 60
};

// --- Content-Format identifiers (RFC 7252 §12.3, CoRE registry) ------------
enum class ContentFormat : uint16_t {
    TextPlain   = 0,
    LinkFormat  = 40,
    Xml         = 41,
    OctetStream = 42,
    Exi         = 47,
    Json        = 50,
    Cbor        = 60
};

// --- Observe option values (RFC 7641 §4) -----------------------------------
constexpr uint32_t kObserveRegister   = 0;
constexpr uint32_t kObserveDeregister = 1;

} // namespace pulsecoap
