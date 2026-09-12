// PulseCoAP — BlockwiseUpload
//
// Demonstrates Block1 (RFC 7959): the CLIENT uploads a payload that is
// larger than one CoAP message. PulseCoAP splits it automatically into
// blocks, sends them one at a time, and calls onResponse when the server
// has accepted the last block.
//
// This sketch is the CLIENT side. Flash it on ESP32-B.
// Flash BlockwiseDownload (or DummyDataServer) on ESP32-A as the server,
// or run a libcoap server on your PC:
//   coap-server -p 5683
//
// Enable block-wise in your build:
//   #define PULSECOAP_ENABLE_BLOCKWISE 1   // before #include <PulseCoAP.h>
//
// What you will see on Serial:
//   Uploading 512-byte config to /config ...
//   [onResponse] Server accepted upload: 2.04 Changed
//   Upload complete.

#define PULSECOAP_ENABLE_BLOCKWISE 1

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";
const char* kServerIP = "192.168.1.8";   // ← IP of the server ESP32 (or PC)

WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(transport, txPool);

pulsecoap::Endpoint serverEp;
bool uploadDone = false;

// --- A large payload (> one CoAP message = 256 bytes by default) --------
// In a real application this would be firmware metadata, a JSON config,
// a certificate, etc.  Here we fill it with a repeating pattern so it is
// easy to verify on the server side.
static const size_t kPayloadLen = 512;
static uint8_t payload[kPayloadLen];

void buildPayload() {
    // Fill with "ABCDEFGHIJ..." repeating
    for (size_t i = 0; i < kPayloadLen; ++i)
        payload[i] = 'A' + (i % 26);
}

// --- Response callback --------------------------------------------------
void onResponse(const pulsecoap::ClientResponse& res, void*) {
    Serial.printf("[onResponse] Server replied: %d.%02d\n",
                  res.code >> 5, res.code & 0x1F);
    if (res.code == pulsecoap::Code::Changed ||
        res.code == pulsecoap::Code::Created) {
        Serial.println("Upload complete — server accepted all blocks.");
    } else {
        Serial.println("Upload failed — unexpected response code.");
    }
    uploadDone = true;
}

// --- setup --------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP BlockwiseUpload ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.printf("\nClient IP: %s\n", WiFi.localIP().toString().c_str());

    serverEp.ip[0]=192; serverEp.ip[1]=168; serverEp.ip[2]=1; serverEp.ip[3]=8;
    serverEp.port = 5683;

    client.begin();
    buildPayload();

    Serial.printf("Uploading %u bytes to /config ...\n", (unsigned)kPayloadLen);

    // client.put() with a payload larger than PULSECOAP_MAX_MSG_SIZE is
    // automatically split into Block1 fragments by the library.
    client.put(serverEp, "/config",
               payload, kPayloadLen,
               pulsecoap::ContentFormat::TextPlain,
               onResponse, nullptr,
               /*confirmable=*/true);
}

// --- loop ---------------------------------------------------------------
void loop() {
    client.poll(millis());
    if (uploadDone) {
        // Nothing left to do — real firmware would start the next task here
        delay(10000);
    }
}
