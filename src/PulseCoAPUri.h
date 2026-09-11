// PulseCoAPUri.h
// Small shared helpers for turning a "/sensors/temp" style path into/from
// CoAP Uri-Path options (RFC 7252 §5.10.1). Used by both Server and Client
// so path handling stays identical on both sides.
#pragma once

#include <string.h>

#include "PulseCoAPMessage.h"
#include "PulseCoAPTypes.h"

namespace pulsecoap {
namespace uri {

// Splits `path` on '/' and adds each non-empty segment as a Uri-Path
// option. Uri-Path is option number 11 — call this before adding any
// option numbered above 11 (Content-Format, etc), since CoAP options must
// be added in ascending option-number order.
inline bool addUriPathOptions(Message& msg, const char* path) {
    if (!path) return true;
    const char* start = path;
    while (*start) {
        while (*start == '/') start++;
        if (!*start) break;
        const char* end = start;
        while (*end && *end != '/') end++;
        uint16_t len = static_cast<uint16_t>(end - start);
        if (!msg.addOption(static_cast<uint16_t>(OptionNumber::UriPath),
                            reinterpret_cast<const uint8_t*>(start), len)) {
            return false;
        }
        start = end;
    }
    return true;
}

// Rebuilds a "/seg1/seg2" path from a decoded message's Uri-Path options
// into `out` (capacity `outCapacity`, always left NUL-terminated on
// success). A message with no Uri-Path options joins to "/". Returns false
// if the reconstructed path wouldn't fit.
inline bool joinUriPath(const Message& msg, char* out, size_t outCapacity) {
    if (outCapacity < 2) return false;
    size_t pos = 0;
    out[0] = '\0';

    for (uint8_t i = 0; i < msg.optionCount(); ++i) {
        const Option* opt = msg.optionAt(i);
        if (!opt || opt->number != static_cast<uint16_t>(OptionNumber::UriPath)) continue;

        if (pos + 1 >= outCapacity) return false;
        out[pos++] = '/';

        if (pos + opt->length >= outCapacity) return false;
        if (opt->length > 0) memcpy(out + pos, opt->value, opt->length);
        pos += opt->length;
        out[pos] = '\0';
    }

    if (pos == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return true;
}

} // namespace uri
} // namespace pulsecoap
