// PulseCoAP — URITemplates
//
// Demonstrates URI-template resource paths.
//
// Instead of registering /sensors/0, /sensors/1, /sensors/2 separately,
// you register ONE path with a :param segment:
//
//   server.addResource("/sensors/:id", MethodGet, handleSensor);
//
// Inside the handler, req.pathParam("id") returns the matched segment.
// Exact paths always have priority — /sensors/special would match a
// separately registered exact resource, not this template.
//
// This sketch runs both SERVER and CLIENT on one ESP32 (loopback over WiFi).
// The server exposes three virtual sensors (id = 0, 1, 2); the client
// queries each by ID.
//
// Test from PC:
//   coap-client -m get coap://<IP>/sensors/0
//   coap-client -m get coap://<IP>/sensors/1
//   coap-client -m get coap://<IP>/sensors/99   # → 4.04 Not Found

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

// ── CoAP stack ────────────────────────────────────────────────────────────
WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server              server(transport);

WiFiUDP                        clientUdp;
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(clientTransport, txPool);

pulsecoap::Endpoint loopbackEp;

// ── Virtual sensor bank ───────────────────────────────────────────────────
struct Sensor { const char* name; float value; };
static const Sensor kSensors[] = {
    { "temperature", 24.3f },
    { "humidity",    61.0f },
    { "pressure",   1013.2f },
};
static const uint8_t kSensorCount = sizeof(kSensors) / sizeof(kSensors[0]);

// ── Server handler — one handler covers ALL sensor IDs ────────────────────
void handleSensor(const pulsecoap::Request& req, pulsecoap::Response& res, void*) {
    const char* idStr = req.pathParam("id");
    if (!idStr) {
        res.code = pulsecoap::Code::BadRequest;
        return;
    }

    int id = atoi(idStr);
    if (id < 0 || id >= (int)kSensorCount) {
        res.code = pulsecoap::Code::NotFound;
        res.setPayload("sensor id out of range");
        Serial.printf("[server] /sensors/%s → 4.04 Not Found\n", idStr);
        return;
    }

    static char buf[32];
    snprintf(buf, sizeof(buf), "%s=%.1f", kSensors[id].name, kSensors[id].value);
    res.code          = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
    Serial.printf("[server] /sensors/%s → %s\n", idStr, buf);
}

// ── Client callbacks ──────────────────────────────────────────────────────
void onSensorResponse(const pulsecoap::ClientResponse& res, void* ctx) {
    int id = (int)(intptr_t)ctx;
    if (res.code == pulsecoap::Code::Content) {
        char buf[res.payloadLength + 1];
        memcpy(buf, res.payload, res.payloadLength);
        buf[res.payloadLength] = '\0';
        Serial.printf("[client] sensor %d: %s\n", id, buf);
    } else {
        Serial.printf("[client] sensor %d: error %d.%02d\n",
                      id, res.code >> 5, res.code & 0x1F);
    }
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP URITemplates ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    auto ip = WiFi.localIP();
    Serial.printf("IP: %s\n", ip.toString().c_str());

    // Server: one template handles /sensors/0, /sensors/1, /sensors/2, ...
    server.addResource("/sensors/:id", pulsecoap::MethodGet, handleSensor);
    server.begin(5683);
    Serial.println("Server: GET /sensors/:id ready");

    // Client: query each sensor by ID (loopback)
    loopbackEp.ip[0]=ip[0]; loopbackEp.ip[1]=ip[1];
    loopbackEp.ip[2]=ip[2]; loopbackEp.ip[3]=ip[3];
    loopbackEp.port = 5683;

    clientUdp.begin(5684);
    client.begin();
    delay(200);

    Serial.println("\nClient querying all sensors...");
    for (int i = 0; i < (int)kSensorCount; ++i) {
        char path[20];
        snprintf(path, sizeof(path), "/sensors/%d", i);
        client.get(loopbackEp, path, onSensorResponse, (void*)(intptr_t)i);
        delay(50); // small gap between requests
    }

    // Also query an invalid ID to show 4.04 handling
    client.get(loopbackEp, "/sensors/99", onSensorResponse, (void*)(intptr_t)99);
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);
}
