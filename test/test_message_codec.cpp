// Host-side test for PulseCoAPMessage's encode()/decode().
// No framework dependency — build directly against a host g++/clang++, e.g.:
//   g++ -std=c++11 -Wall -Wextra -Isrc test/test_message_codec.cpp src/PulseCoAPMessage.cpp -o test_message_codec
//   ./test_message_codec
#include <cstdio>
#include <cstring>

#include "../src/PulseCoAPMessage.h"

using namespace pulsecoap;

namespace {
int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}
} // namespace

#define CHECK(cond) check((cond), #cond)

static void test_round_trip_get_request() {
    std::printf("test_round_trip_get_request\n");

    Message msg;
    msg.setType(MessageType::Confirmable);
    msg.setCode(Code::Get);
    msg.setMessageId(0x1234);

    const uint8_t token[] = {0xAB, 0xCD};
    CHECK(msg.setToken(token, sizeof(token)));

    // /temp/value  -> two Uri-Path options, both option number 11
    CHECK(msg.addOptionString(static_cast<uint16_t>(OptionNumber::UriPath), "temp"));
    CHECK(msg.addOptionString(static_cast<uint16_t>(OptionNumber::UriPath), "value"));
    // ?raw=1
    CHECK(msg.addOptionString(static_cast<uint16_t>(OptionNumber::UriQuery), "raw=1"));

    uint8_t buf[64];
    size_t len = msg.encode(buf, sizeof(buf));
    CHECK(len > 0);

    Message decoded;
    DecodeError err = decoded.decode(buf, len);
    CHECK(err == DecodeError::None);

    CHECK(decoded.type() == MessageType::Confirmable);
    CHECK(decoded.code() == Code::Get);
    CHECK(decoded.messageId() == 0x1234);
    CHECK(decoded.tokenLength() == 2);
    CHECK(decoded.token()[0] == 0xAB && decoded.token()[1] == 0xCD);
    CHECK(decoded.optionCount() == 3);

    const Option* o0 = decoded.optionAt(0);
    const Option* o1 = decoded.optionAt(1);
    const Option* o2 = decoded.optionAt(2);
    CHECK(o0 && o0->number == static_cast<uint16_t>(OptionNumber::UriPath) &&
          o0->length == 4 && std::memcmp(o0->value, "temp", 4) == 0);
    CHECK(o1 && o1->number == static_cast<uint16_t>(OptionNumber::UriPath) &&
          o1->length == 5 && std::memcmp(o1->value, "value", 5) == 0);
    CHECK(o2 && o2->number == static_cast<uint16_t>(OptionNumber::UriQuery) &&
          o2->length == 5 && std::memcmp(o2->value, "raw=1", 5) == 0);
    CHECK(decoded.payloadLength() == 0);
}

static void test_round_trip_response_with_payload_and_uint_options() {
    std::printf("test_round_trip_response_with_payload_and_uint_options\n");

    Message msg;
    msg.setType(MessageType::Acknowledgement);
    msg.setCode(Code::Content);
    msg.setMessageId(0x0001);
    // Empty token is legal (TKL=0)
    CHECK(msg.setToken(nullptr, 0));

    // Must be added in ascending option-number order: Observe(6) < Content-Format(12) < Max-Age(14).
    CHECK(msg.addOptionUint(static_cast<uint16_t>(OptionNumber::Observe), 0)); // value 0 -> zero-length option
    CHECK(msg.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                             static_cast<uint32_t>(ContentFormat::Json)));
    CHECK(msg.addOptionUint(static_cast<uint16_t>(OptionNumber::MaxAge), 60));

    const char* payload = "{\"t\":21.5}";
    CHECK(msg.setPayload(reinterpret_cast<const uint8_t*>(payload), std::strlen(payload)));

    uint8_t buf[64];
    size_t len = msg.encode(buf, sizeof(buf));
    CHECK(len > 0);

    Message decoded;
    DecodeError err = decoded.decode(buf, len);
    CHECK(err == DecodeError::None);
    CHECK(decoded.tokenLength() == 0);
    CHECK(decoded.optionCount() == 3);

    const Option* cf = decoded.findOption(static_cast<uint16_t>(OptionNumber::ContentFormat));
    CHECK(cf && Message::optionAsUint(*cf) == static_cast<uint32_t>(ContentFormat::Json));

    const Option* obs = decoded.findOption(static_cast<uint16_t>(OptionNumber::Observe));
    CHECK(obs && obs->length == 0 && Message::optionAsUint(*obs) == 0);

    const Option* maxAge = decoded.findOption(static_cast<uint16_t>(OptionNumber::MaxAge));
    CHECK(maxAge && Message::optionAsUint(*maxAge) == 60);

    CHECK(decoded.payloadLength() == std::strlen(payload));
    CHECK(std::memcmp(decoded.payload(), payload, decoded.payloadLength()) == 0);
}

static void test_extended_option_numbers() {
    std::printf("test_extended_option_numbers\n");

    // Exercise the 13-and-269 extended-delta/length boundaries directly on
    // the wire, independent of any named option, to prove the codec's
    // nibble/extended-byte math rather than just typical small options.
    Message msg;
    msg.setType(MessageType::NonConfirmable);
    msg.setCode(Code::Content);
    msg.setMessageId(7);

    uint8_t big[300];
    for (int i = 0; i < 300; ++i) big[i] = static_cast<uint8_t>(i & 0xFF);

    // option number 20 (>= 13, needs 1 extended delta byte), 300-byte value
    // (>= 269, needs 2 extended length bytes).
    CHECK(msg.addOption(20, big, sizeof(big)));
    // next option number 400 (delta = 380, >= 269 => 2 extended delta bytes)
    CHECK(msg.addOption(400, reinterpret_cast<const uint8_t*>("x"), 1));

    uint8_t buf[512];
    size_t len = msg.encode(buf, sizeof(buf));
    CHECK(len > 0);

    Message decoded;
    DecodeError err = decoded.decode(buf, len);
    CHECK(err == DecodeError::None);
    CHECK(decoded.optionCount() == 2);

    const Option* o0 = decoded.optionAt(0);
    CHECK(o0 && o0->number == 20 && o0->length == 300 &&
          std::memcmp(o0->value, big, 300) == 0);

    const Option* o1 = decoded.optionAt(1);
    CHECK(o1 && o1->number == 400 && o1->length == 1 && o1->value[0] == 'x');
}

static void test_rejects_out_of_order_options() {
    std::printf("test_rejects_out_of_order_options\n");
    Message msg;
    CHECK(msg.addOption(11, reinterpret_cast<const uint8_t*>("b"), 1));
    // number 5 < previous 11 -> must be rejected, delta encoding can't go backwards
    CHECK(!msg.addOption(5, reinterpret_cast<const uint8_t*>("a"), 1));
    CHECK(msg.optionCount() == 1);
}

static void test_decode_rejects_malformed_input() {
    std::printf("test_decode_rejects_malformed_input\n");

    Message decoded;

    // Too short: fewer than 4 header bytes
    const uint8_t tooShort[] = {0x40, 0x01, 0x00};
    CHECK(decoded.decode(tooShort, sizeof(tooShort)) == DecodeError::TooShort);

    // Bad version: Ver field (top 2 bits of byte 0) must be 1
    const uint8_t badVersion[] = {0x00, 0x01, 0x00, 0x00}; // Ver=0
    CHECK(decoded.decode(badVersion, sizeof(badVersion)) == DecodeError::BadVersion);

    // Payload marker with nothing after it
    const uint8_t emptyPayload[] = {0x40, 0x01, 0x00, 0x00, 0xFF};
    CHECK(decoded.decode(emptyPayload, sizeof(emptyPayload)) == DecodeError::EmptyPayloadMarker);

    // TKL says 4 token bytes but message ends right after the header
    const uint8_t truncatedToken[] = {0x44, 0x01, 0x00, 0x00, 0xAA, 0xBB};
    CHECK(decoded.decode(truncatedToken, sizeof(truncatedToken)) == DecodeError::TooShort);
}

static void test_role_selection_macros_compile() {
    // This is a compile-time check, not a runtime one: as long as this
    // translation unit built, PULSECOAP_ENABLE_CLIENT/SERVER resolved to 0/1
    // without error under the default (both-roles) configuration.
    std::printf("test_role_selection_macros_compile\n");
    CHECK(PULSECOAP_ENABLE_CLIENT == 1);
    CHECK(PULSECOAP_ENABLE_SERVER == 1);
}

int main() {
    test_round_trip_get_request();
    test_round_trip_response_with_payload_and_uint_options();
    test_extended_option_numbers();
    test_rejects_out_of_order_options();
    test_decode_rejects_malformed_input();
    test_role_selection_macros_compile();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
