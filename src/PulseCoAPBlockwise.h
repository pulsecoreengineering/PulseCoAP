// PulseCoAPBlockwise.h
// Block-wise transfer helpers (RFC 7959 Block1/Block2).
// Only compiled when PULSECOAP_ENABLE_BLOCKWISE is set to 1.
#pragma once

#include "PulseCoAPConfig.h"

#if PULSECOAP_ENABLE_BLOCKWISE

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "PulseCoAPTypes.h"
#include "PulseCoAPMessage.h"
#include "PulseCoAPTransport.h"

namespace pulsecoap {

// ---------------------------------------------------------------------------
// Block option encoding (RFC 7959 §2.2)
//
//   option-value = (NUM << 4) | (M << 3) | SZX
//   block-size   = 2^(SZX + 4)   bytes
// ---------------------------------------------------------------------------

struct BlockOption {
    uint32_t num = 0;
    bool     m   = false;
    uint8_t  szx = PULSECOAP_BLOCK_SZX;

    uint16_t blockSize()   const { return static_cast<uint16_t>(1u << (szx + 4)); }
    uint32_t byteOffset()  const { return num * static_cast<uint32_t>(blockSize()); }

    // Decode a Block1 or Block2 option value into this struct.
    // Returns false if the option length is > 3 (malformed).
    static bool fromOption(const Option& opt, BlockOption& out) {
        if (opt.length > 3) return false;
        uint32_t v = Message::optionAsUint(opt);
        out.szx = static_cast<uint8_t>(v & 0x07u);
        out.m   = static_cast<bool>((v >> 3) & 1u);
        out.num = v >> 4;
        return true;
    }

    // Encode NUM, M, and SZX into the compact option integer.
    static uint32_t toOptionValue(uint32_t num, bool m, uint8_t szx) {
        return (num << 4) | (m ? 0x08u : 0u) | (szx & 0x07u);
    }
};

// ---------------------------------------------------------------------------
// Server-side Block1 reassembly session
// ---------------------------------------------------------------------------

struct Block1Session {
    bool     active            = false;
    Endpoint remote;
    uint8_t  token[PULSECOAP_MAX_TOKEN_LEN] = {0};
    uint8_t  tokenLen          = 0;
    uint32_t nextExpectedNum   = 0;   // next block number we expect
    uint32_t assembledLen      = 0;   // bytes accumulated so far
    uint8_t  buffer[PULSECOAP_BLOCK1_MAX_BODY];
};

// ---------------------------------------------------------------------------
// Client-side Block1 upload state (per pending slot)
// The caller's payload buffer must remain valid for the entire upload.
// ---------------------------------------------------------------------------

struct Block1UploadState {
    bool           active        = false;
    const uint8_t* fullPayload   = nullptr;
    size_t         fullLen       = 0;
    uint32_t       nextNum       = 0;   // block number to send next (on 2.31 Continue)
    uint8_t        szx           = PULSECOAP_BLOCK_SZX;
    ContentFormat  contentFormat = ContentFormat::TextPlain;
    Code           method        = Code::Put;   // Put or Post
    Endpoint       server;
    char           path[PULSECOAP_MAX_URI_PATH_LEN];
    uint8_t        token         = 0;   // consistent token for the whole upload
    bool           waitingForContinue = false;
};

// ---------------------------------------------------------------------------
// Client-side Block2 receive state (per pending slot)
// Accumulates blocks arriving from the server and fires the callback once
// the final block (M=0) arrives.
// ---------------------------------------------------------------------------

struct Block2RecvState {
    bool          active        = false;
    uint32_t      nextNum       = 0;
    uint8_t       szx           = PULSECOAP_BLOCK_SZX;
    uint32_t      assembledLen  = 0;
    ContentFormat contentFormat = ContentFormat::TextPlain;
    Endpoint      server;
    char          path[PULSECOAP_MAX_URI_PATH_LEN];
    uint8_t       buffer[PULSECOAP_BLOCK2_MAX_BODY];
};

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_BLOCKWISE
