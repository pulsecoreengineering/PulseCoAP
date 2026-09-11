// PulseCoAP BasicServer
//
// Exposes GET /sensors/temp on an ESP32 over CoAP (UDP port 5683, the
// standard CoAP port). Try it with any CoAP client, e.g. libcoap's
// coap-client:
//   coap-client -m get coap://<device-ip>/sensors/temp
#include <PulseCoAP.h>
#include <WiFi.h>
#include <WiFiUdp.h>

const char* kWifiSsid = "YOUR_WIFI_SSID";
const char* kWifiPassword = "YOUR_WIFI_PASSWORD";

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server server(transport);

// A fake sensor reading. Swap for a real ADC/I2C read in your own project.
float readTemperatureC() {
    return 21.5f + (millis() % 1000) / 1000.0f;
}

void handleGetTemp(const pulsecoap::Request& /*req*/, pulsecoap::Response& res, void* /*ctx*/) {
    static char payload[16];
    dtostrf(readTemperatureC(), 0, 1, payload);
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(payload);
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(kWifiSsid, kWifiPassword);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print(".");
    }
    Serial.print("\nWiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    if (!server.begin(5683)) {
        Serial.println("Failed to bind CoAP port 5683");
        while (true) delay(1000);
    }

    server.addResource("/sensors/temp", pulsecoap::MethodGet, handleGetTemp);
    Serial.println("PulseCoAP server ready: GET /sensors/temp");
}

void loop() {
    server.poll(millis());
}
