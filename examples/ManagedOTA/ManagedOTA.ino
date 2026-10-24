// ManagedOTA: hands-off updates from the library's own task.
//
// simpleota_start() runs a background FreeRTOS task that checks for updates
// on an interval and installs them. The sketch's loop() is untouched.
//
// The library does not manage Wi-Fi. The sketch brings it up, and the
// is_connected callback lets the task wait for an IP before each check.

#include <WiFi.h>
#include <simpleota.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

// The version label of this image, shown on the dashboard. Bump it with each
// build you upload to SimpleOTA.
#define FIRMWARE_VERSION "1.0.0"

// Polled once a second; the task only checks while it returns true.
static bool wifiUp(void*) {
    return WiFi.status() == WL_CONNECTED;
}

// Runs on the OTA task: keep it short.
static void onOtaEvent(const simpleota_event_t* e, void*) {
    if (e->id == SIMPLEOTA_EVENT_UPDATE_FAILED) {
        Serial.printf("[app] OTA failed: %s\n", e->reason ? e->reason : "?");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[app] Wi-Fi starting; the OTA task waits for an IP.");

    // Every new image runs as a trial. In managed mode the task confirms it
    // on its first successful check-in, which proves boot, connectivity, TLS
    // and the token. To confirm after your own health check instead, set
    // cfg.manual_confirm = true and call simpleota_confirm(): see
    // examples/RollbackOTA.
    simpleota_config_t cfg = {};
    cfg.token = SOTA_TOKEN;
    cfg.version_label = FIRMWARE_VERSION;
    cfg.check_interval_s = 3600;
    cfg.is_connected = wifiUp;
    cfg.event_cb = onOtaEvent;
    if (simpleota_init(&cfg) != ESP_OK || simpleota_start() != ESP_OK) {
        Serial.println("[app] SimpleOTA did not start; see the log above");
    }
}

void loop() {
    // Your application code runs here; updates happen in the background.
    delay(1000);
}
