// PulseCoAP — TwoDevicePeerToPeer  (CLIENT sketch)
//
// Flash this on ESP32-B.
// Flash TwoDevicePeerToPeer_Server on ESP32-A first, then copy its IP
// into kServerIP below.
//
// What this client does:
//   1. GETs /.well-known/core to discover what the server has
//   2. Observes /data/temp — prints every push (every 3 s)
//   3. Sends PUT /control/led "on" every 10 s and "off" 5 s later

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";
const char* kServerIP = "192.168.1.X";  // ← paste ESP32-A's IP here

WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(transport, txPool);

pulsecoap::Endpoint serverEp;

// ── Callbacks ─────────────────────────────────────────────────────────────
void onDiscovery(const pulsecoap::ClientResponse& res, void*) {
    Serial.println("\n[discovery] /.well-known/core:");
    Serial.write(res.payload, res.payloadLength);
    Serial.println();
}

void onTemp(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength); buf[res.payloadLength] = '\0';
    Serial.printf("[observe /data/temp] %s °C\n", buf);
}

void onLedResponse(const pulsecoap::ClientResponse& res, void* ctx) {
    const char* state = (const char*)ctx;
    if (res.code == pulsecoap::Code::Changed) {
        Serial.printf("[led] PUT '%s' accepted\n", state);
    } else {
        Serial.printf("[led] PUT '%s' failed: %d.%02d\n",
                      state, res.code >> 5, res.code & 0x1F);
    }
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP P2P Client ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.printf("Client IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Server IP: %s\n\n", kServerIP);

    // Parse server IP string into Endpoint
    IPAddress addr; addr.fromString(kServerIP);
    serverEp.ip[0]=addr[0]; serverEp.ip[1]=addr[1];
    serverEp.ip[2]=addr[2]; serverEp.ip[3]=addr[3];
    serverEp.port = 5683;

    client.begin();
    delay(300);

    // 1. Discover what the server has
    Serial.println("Step 1: resource discovery...");
    client.get(serverEp, "/.well-known/core", onDiscovery);

    // 2. Subscribe to temperature
    Serial.println("Step 2: subscribing to /data/temp ...");
    client.observe(serverEp, "/data/temp", onTemp, nullptr);
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    client.poll(now);

    // 3. Toggle LED every 10 s: on at T=10, off at T=15, on at T=20, ...
    static uint32_t lastToggle = 0;
    static bool     ledOn      = false;

    if (now - lastToggle >= (ledOn ? 5000u : 10000u)) {
        lastToggle = now;
        ledOn = !ledOn;
        const char* payload = ledOn ? "on" : "off";
        Serial.printf("[led] sending PUT /control/led '%s'\n", payload);
        client.put(serverEp, "/control/led",
                   reinterpret_cast<const uint8_t*>(payload), strlen(payload),
                   pulsecoap::ContentFormat::TextPlain,
                   onLedResponse, (void*)payload,
                   /*confirmable=*/true);
    }
}
