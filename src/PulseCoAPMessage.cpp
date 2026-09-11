#include "PulseCoAPMessage.h"

#include <string.h>

namespace pulsecoap {

namespace {

// Encodes a 16-bit option delta or length into its 4-bit nibble plus 0-2
// extended bytes, per RFC 7252 §3.1's "Option Delta"/"Option Length" table.
uint8_t encodeOptionField(uint16_t value, uint8_t extra[2], uint8_t& extraLen) {
    if (value < 13) {
        extraLen = 0;
        return static_cast<uint8_t>(value);
    } else if (value < 269) {
        extra[0] = static_cast<uint8_t>(value - 13);
        extraLen = 1;
        return 13;
    } else {
        uint16_t v = static_cast<uint16_t>(value - 269);
        extra[0] = static_cast<uint8_t>(v >> 8);
        extra[1] = static_cast<uint8_t>(v & 0xFF);
        extraLen = 2;
        return 14;
    }
}

} // namespace

void Message::reset() {
    type_ = MessageType::Confirmable;
    code_ = Code::Empty;
    messageId_ = 0;
    tokenLen_ = 0;
    optionCount_ = 0;
    lastOptionNumber_ = 0;
    payload_ = nullptr;
    payloadLen_ = 0;
}

bool Message::setToken(const uint8_t* token, uint8_t len) {
    if (len > PULSECOAP_MAX_TOKEN_LEN) return false;
    for (uint8_t i = 0; i < len; ++i) token_[i] = token[i];
    tokenLen_ = len;
    return true;
}

bool Message::addOption(uint16_t number, const uint8_t* value, uint16_t length) {
    if (optionCount_ >= PULSECOAP_MAX_OPTIONS) return false;
    // CoAP options are delta-encoded from the previous option's number, so
    // they must be added in ascending numeric order.
    if (optionCount_ > 0 && number < lastOptionNumber_) return false;

    options_[optionCount_].number = number;
    options_[optionCount_].length = length;
    options_[optionCount_].value = length > 0 ? value : nullptr;
    optionCount_++;
    lastOptionNumber_ = number;
    return true;
}

bool Message::addOptionString(uint16_t number, const char* value) {
    size_t len = value ? strlen(value) : 0;
    return addOption(number, reinterpret_cast<const uint8_t*>(value), static_cast<uint16_t>(len));
}

bool Message::addOptionUint(uint16_t number, uint32_t value) {
    if (optionCount_ >= PULSECOAP_MAX_OPTIONS) return false;

    // RFC 7252 §3.2: uint options are encoded big-endian with all leading
    // zero bytes stripped; the value 0 is encoded as a zero-length option.
    uint8_t little[4];
    uint8_t n = 0;
    uint32_t v = value;
    while (v > 0 && n < 4) {
        little[n++] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }

    uint8_t slot = optionCount_; // addOption() below will use this same index
    for (uint8_t i = 0; i < n; ++i) {
        uintScratch_[slot][i] = little[n - 1 - i];
    }

    return addOption(number, n > 0 ? uintScratch_[slot] : nullptr, n);
}

const Option* Message::optionAt(uint8_t index) const {
    if (index >= optionCount_) return nullptr;
    return &options_[index];
}

const Option* Message::findOption(uint16_t number) const {
    for (uint8_t i = 0; i < optionCount_; ++i) {
        if (options_[i].number == number) return &options_[i];
    }
    return nullptr;
}

uint32_t Message::optionAsUint(const Option& opt) {
    uint32_t v = 0;
    uint16_t len = opt.length > 4 ? 4 : opt.length;
    for (uint16_t i = 0; i < len; ++i) {
        v = (v << 8) | opt.value[i];
    }
    return v;
}

bool Message::setPayload(const uint8_t* data, size_t length) {
    payload_ = data;
    payloadLen_ = length;
    return true;
}

size_t Message::encode(uint8_t* out, size_t outCapacity) const {
    size_t pos = 0;

    auto put = [&](uint8_t b) -> bool {
        if (pos >= outCapacity) return false;
        out[pos++] = b;
        return true;
    };
    auto putBytes = [&](const uint8_t* data, size_t len) -> bool {
        if (len == 0) return true;
        if (pos + len > outCapacity) return false;
        for (size_t i = 0; i < len; ++i) out[pos + i] = data[i];
        pos += len;
        return true;
    };

    if (outCapacity < 4) return 0;

    uint8_t byte0 = static_cast<uint8_t>((kVersion << 6) |
                                          ((static_cast<uint8_t>(type_) & 0x03) << 4) |
                                          (tokenLen_ & 0x0F));
    if (!put(byte0)) return 0;
    if (!put(static_cast<uint8_t>(code_))) return 0;
    if (!put(static_cast<uint8_t>(messageId_ >> 8))) return 0;
    if (!put(static_cast<uint8_t>(messageId_ & 0xFF))) return 0;
    if (!putBytes(token_, tokenLen_)) return 0;

    uint16_t lastNumber = 0;
    for (uint8_t i = 0; i < optionCount_; ++i) {
        const Option& opt = options_[i];
        uint16_t delta = static_cast<uint16_t>(opt.number - lastNumber);
        lastNumber = opt.number;

        uint8_t deltaExtra[2];
        uint8_t deltaExtraLen;
        uint8_t deltaNibble = encodeOptionField(delta, deltaExtra, deltaExtraLen);

        uint8_t lengthExtra[2];
        uint8_t lengthExtraLen;
        uint8_t lengthNibble = encodeOptionField(opt.length, lengthExtra, lengthExtraLen);

        if (!put(static_cast<uint8_t>((deltaNibble << 4) | lengthNibble))) return 0;
        if (!putBytes(deltaExtra, deltaExtraLen)) return 0;
        if (!putBytes(lengthExtra, lengthExtraLen)) return 0;

        if (opt.length > 0) {
            if (!opt.value) return 0; // programmer error: non-zero length, null buffer
            if (!putBytes(opt.value, opt.length)) return 0;
        }
    }

    if (payload_ && payloadLen_ > 0) {
        if (!put(0xFF)) return 0;
        if (!putBytes(payload_, payloadLen_)) return 0;
    }

    return pos;
}

DecodeError Message::decode(const uint8_t* in, size_t inLength) {
    reset();

    if (inLength < 4) return DecodeError::TooShort;

    uint8_t byte0 = in[0];
    uint8_t version = static_cast<uint8_t>(byte0 >> 6);
    if (version != kVersion) return DecodeError::BadVersion;

    type_ = static_cast<MessageType>((byte0 >> 4) & 0x03);
    uint8_t tkl = static_cast<uint8_t>(byte0 & 0x0F);
    if (tkl > PULSECOAP_MAX_TOKEN_LEN) return DecodeError::BadTokenLength;

    code_ = static_cast<Code>(in[1]);
    messageId_ = static_cast<uint16_t>((static_cast<uint16_t>(in[2]) << 8) | in[3]);

    size_t pos = 4;
    if (pos + tkl > inLength) { reset(); return DecodeError::TooShort; }
    for (uint8_t i = 0; i < tkl; ++i) token_[i] = in[pos + i];
    tokenLen_ = tkl;
    pos += tkl;

    uint16_t lastNumber = 0;
    while (pos < inLength) {
        uint8_t byte = in[pos];

        if (byte == 0xFF) {
            pos++;
            if (pos >= inLength) { reset(); return DecodeError::EmptyPayloadMarker; }
            payload_ = &in[pos];
            payloadLen_ = inLength - pos;
            pos = inLength;
            break;
        }

        uint8_t deltaNibble = static_cast<uint8_t>(byte >> 4);
        uint8_t lengthNibble = static_cast<uint8_t>(byte & 0x0F);
        pos++;

        // Nibble 15 is reserved and only legal as part of the full 0xFF
        // payload marker byte, already handled above.
        if (deltaNibble == 15 || lengthNibble == 15) { reset(); return DecodeError::BadOption; }

        uint16_t delta;
        if (deltaNibble < 13) {
            delta = deltaNibble;
        } else if (deltaNibble == 13) {
            if (pos >= inLength) { reset(); return DecodeError::TooShort; }
            delta = static_cast<uint16_t>(13 + in[pos]);
            pos += 1;
        } else { // 14
            if (pos + 1 >= inLength) { reset(); return DecodeError::TooShort; }
            delta = static_cast<uint16_t>(269 + ((static_cast<uint16_t>(in[pos]) << 8) | in[pos + 1]));
            pos += 2;
        }

        uint16_t length;
        if (lengthNibble < 13) {
            length = lengthNibble;
        } else if (lengthNibble == 13) {
            if (pos >= inLength) { reset(); return DecodeError::TooShort; }
            length = static_cast<uint16_t>(13 + in[pos]);
            pos += 1;
        } else { // 14
            if (pos + 1 >= inLength) { reset(); return DecodeError::TooShort; }
            length = static_cast<uint16_t>(269 + ((static_cast<uint16_t>(in[pos]) << 8) | in[pos + 1]));
            pos += 2;
        }

        uint16_t optionNumber = static_cast<uint16_t>(lastNumber + delta);
        lastNumber = optionNumber;

        if (pos + length > inLength) { reset(); return DecodeError::TooShort; }
        if (optionCount_ >= PULSECOAP_MAX_OPTIONS) { reset(); return DecodeError::TooManyOptions; }

        options_[optionCount_].number = optionNumber;
        options_[optionCount_].length = length;
        options_[optionCount_].value = (length > 0) ? &in[pos] : nullptr;
        optionCount_++;
        pos += length;
    }

    return DecodeError::None;
}

} // namespace pulsecoap
