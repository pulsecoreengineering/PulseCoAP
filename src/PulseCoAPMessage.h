// PulseCoAPMessage.h
// No-heap CoAP message encoder/decoder (RFC 7252 §3).
//
// Design notes:
//  - Option values added via addOption()/addOptionString() are NOT copied —
//    Message stores a pointer into the caller-supplied buffer. That buffer
//    (a string literal, a local array, a resource's own state) must stay
//    valid until encode() has run. This mirrors the no-heap/no-copy
//    discipline used across the Pulse library ecosystem.
//  - addOptionUint() is the one exception: it copies the minimal big-endian
//    encoding of the value into a small internal scratch table (max 4 bytes
//    per option), since callers rarely keep an lvalue around just to encode
//    an integer option.
//  - decode() does not copy either: option values and the payload point
//    directly into the buffer you pass in. Keep that buffer alive for as
//    long as you read from the decoded Message.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "PulseCoAPConfig.h"
#include "PulseCoAPTypes.h"

namespace pulsecoap {

struct Option {
    uint16_t number = 0;
    uint16_t length = 0;
    const uint8_t* value = nullptr;
};

enum class DecodeError : uint8_t {
    None = 0,
    TooShort,          // fewer than 4 header bytes, or truncated token/option/payload
    BadVersion,        // Ver field wasn't 1
    BadTokenLength,    // TKL in the 9-15 reserved range
    BadOption,         // malformed option delta/length encoding
    TooManyOptions,    // more options than PULSECOAP_MAX_OPTIONS
    EmptyPayloadMarker // 0xFF present with zero payload bytes after it (RFC 7252 §3)
};

class Message {
public:
    Message() { reset(); }

    // Clears all fields back to an empty CON/EMPTY message with no
    // token/options/payload. Does not touch caller-owned option/payload
    // buffers (there aren't any left to touch).
    void reset();

    // --- header ---
    void setType(MessageType t) { type_ = t; }
    MessageType type() const { return type_; }

    void setCode(Code c) { code_ = c; }
    Code code() const { return code_; }

    void setMessageId(uint16_t id) { messageId_ = id; }
    uint16_t messageId() const { return messageId_; }

    // len must be 0-8 (RFC 7252 §3). Returns false and leaves the token
    // unchanged if len is out of range.
    bool setToken(const uint8_t* token, uint8_t len);
    const uint8_t* token() const { return token_; }
    uint8_t tokenLength() const { return tokenLen_; }

    // --- options ---
    // Options MUST be added in ascending option-number order — CoAP encodes
    // each option as a delta from the previous option's number, so this
    // isn't just a style preference, it's required for correct encoding.
    // Returns false (and adds nothing) if the table is full.
    bool addOption(uint16_t number, const uint8_t* value, uint16_t length);
    bool addOptionString(uint16_t number, const char* value);
    bool addOptionUint(uint16_t number, uint32_t value);
    bool addOptionEmpty(uint16_t number) { return addOption(number, nullptr, 0); }

    uint8_t optionCount() const { return optionCount_; }
    const Option* optionAt(uint8_t index) const;
    // First option matching `number`, or nullptr. For repeatable options
    // (e.g. Uri-Path) walk optionAt()/optionCount() yourself.
    const Option* findOption(uint16_t number) const;
    static uint32_t optionAsUint(const Option& opt);

    // --- payload ---
    // Not copied — see class-level note above.
    bool setPayload(const uint8_t* data, size_t length);
    const uint8_t* payload() const { return payload_; }
    size_t payloadLength() const { return payloadLen_; }

    // --- wire format ---
    // Encodes into `out` (capacity `outCapacity`). Returns the number of
    // bytes written, or 0 if it wouldn't fit.
    size_t encode(uint8_t* out, size_t outCapacity) const;

    // Decodes `in` (length `inLength`), replacing this message's contents.
    // On failure the message is left reset() and the error is returned.
    // `in` must stay valid for as long as you read options/payload back.
    DecodeError decode(const uint8_t* in, size_t inLength);

private:
    MessageType type_;
    Code code_;
    uint16_t messageId_;

    uint8_t token_[PULSECOAP_MAX_TOKEN_LEN];
    uint8_t tokenLen_;

    Option options_[PULSECOAP_MAX_OPTIONS];
    uint8_t optionCount_;
    uint16_t lastOptionNumber_; // highest option number added so far (encode-side ordering check)

    // Scratch storage for addOptionUint()'s minimal big-endian encoding.
    // Sized 4 bytes/option (uint32 max) — small and fixed regardless of
    // PULSECOAP_MAX_MSG_SIZE.
    uint8_t uintScratch_[PULSECOAP_MAX_OPTIONS][4];

    const uint8_t* payload_;
    size_t payloadLen_;
};

} // namespace pulsecoap
