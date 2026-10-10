// Compiles the library on every chip the core supports.
// ESP32-H2 has no Wi-Fi radio, so this sketch does not include WiFi.h.

#include <SimpleOTAClient.h>

SimpleOTAClient ota("token", SimpleOTAClient::CHIP_ESP32);

void setup() {
    ota.confirmRunning();
}

void loop() {}
