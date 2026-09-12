// PulseCoAP — BlockwiseDownload
//
// Demonstrates Block2 (RFC 7959): the SERVER holds a large payload and
// the CLIENT fetches it. The server auto-fragments the response into
// blocks; the client reassembles them and fires onResponse once with
// the complete payload.
//
// This single sketch runs BOTH roles on one ESP32 using an in-memory
// loopback — useful for testing without a second board. To split across
// two boards, move the Server section to one sketch and the Client
// section to another, and replace LoopbackTransport with ArduinoUdpTransport.
//
// Enable block-wise in your build:
//   #define PULSECOAP_ENABLE_BLOCKWISE 1   // before #include <PulseCoAP.h>
//
// What you will see on Serial:
//   Server: registered /firmware/info  (648 bytes)
//   Client: sending GET /firmware/info ...
//   Client: received 648 bytes (reassembled from N blocks)
//   First 64 bytes: {"version":"1.2.3","build":"2026-09-12", ...

#define PULSECOAP_ENABLE_BLOCKWISE 1

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

// ── A large static payload the server will serve ────────────────────────
// In a real application this would be a JSON config, a firmware manifest,
// a log dump, etc.  Here we use a fake JSON-like blob > 256 bytes so the
// library is forced to use at least two Block2 fragments.
static const char kLargePayload[] =
    "{"
    "\"version\":\"1.2.3\","
    "\"build\":\"2026-09-12\","
    "\"board\":\"ESP32-WROOM-32\","
    "\"features\":[\"coap\",\"observe\",\"blockwise\",\"ipv6\"],"
    "\"limits\":{"
        "\"maxMsgSize\":256,"
        "\"maxResources\":8,"
        "\"maxObservers\":4,"
        "\"maxTransactions\":4"
    "},"
    "\"transport\":\"ArduinoUDP\","
    "\"uptime\":0,"
    "\"status\":\"ok\","
    "\"description\":\"PulseCoAP BlockwiseDownload demo payload — "
        "this string is intentionally long so the library must split "
        "the response into multiple Block2 fragments for reassembly.\""
    "}";

// ── CoAP stack ──────────────────────────────────────────────────────────
WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server              server(transport);

WiFiUDP                        clientUdp;
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(clientTransport, txPool);

pulsecoap::Endpoint serverEp;
bool fetchDone = false;

// ── Server handler ──────────────────────────────────────────────────────
void handleFirmwareInfo(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    res.code          = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::AppJSON;
    res.setPayload(kLargePayload);
    // The library sees payloadLength > block size and automatically
    // fragments the response using Block2 — no extra code needed here.
}

// ── Client callback ─────────────────────────────────────────────────────
void onFirmwareInfo(const pulsecoap::ClientResponse& res, void*) {
    Serial.printf("\nClient: received %u bytes (reassembled from blocks)\n",
                  (unsigned)res.payloadLength);

    // Print first 128 bytes so you can verify the content
    size_t preview = res.payloadLength < 128 ? res.payloadLength : 128;
    Serial.print("Preview: ");
    Serial.write(res.payload, preview);
    Serial.println(res.payloadLength > 128 ? "..." : "");
    fetchDone = true;
}

// ── setup ───────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP BlockwiseDownload ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    // Server
    server.addResource("/firmware/info",
                       pulsecoap::MethodGet, handleFirmwareInfo);
    server.begin(5683);
    Serial.printf("Server: /firmware/info registered (%u bytes)\n",
                  (unsigned)(sizeof(kLargePayload) - 1));

    // Point client at the server (same device — loopback over WiFi)
    WiFi.localIP().toString(); // already printed
    auto ip = WiFi.localIP();
    serverEp.ip[0] = ip[0]; serverEp.ip[1] = ip[1];
    serverEp.ip[2] = ip[2]; serverEp.ip[3] = ip[3];
    serverEp.port  = 5683;

    clientUdp.begin(5684); // client binds a different port
    client.begin();

    delay(200); // let server settle
    Serial.println("Client: GET /firmware/info ...");
    client.get(serverEp, "/firmware/info", onFirmwareInfo);
}

// ── loop ────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);
    if (fetchDone) delay(10000);
}
