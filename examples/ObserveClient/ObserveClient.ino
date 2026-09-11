// PulseCoAP ObserveClient
//
// Registers Observe on /sensors/temp (RFC 7641) so the server pushes every
// update instead of this device having to poll — the main reason to reach
// for CoAP over a simple request/response protocol on a battery device.
#include <PulseCoAP.h>
#include <WiFi.h>
#include <WiFiUdp.h>

const char* kWifiSsid = "YOUR_WIFI_SSID";
const char* kWifiPassword = "YOUR_WIFI_PASSWORD";

pulsecoap::Endpoint kServer = {{192, 168, 1, 42}, 5683};

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::TransactionPool transactions;
pulsecoap::Client client(transport, transactions);

void onTempUpdate(const pulsecoap::ClientResponse& res, void* /*ctx*/) {
    Serial.print("Temp update: ");
    Serial.write(res.payload, res.payloadLength);
    Serial.println();
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

    client.begin();

    if (client.observe(kServer, "/sensors/temp", onTempUpdate)) {
        Serial.println("Observe registration sent for /sensors/temp");
    }
}

void loop() {
    // Nothing to poll for on a timer here — updates arrive whenever the
    // server calls notify(), which poll() picks up as soon as it's called.
    client.poll(millis());
}
