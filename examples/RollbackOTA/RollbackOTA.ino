// RollbackOTA: managed updates, confirmed by the sketch's own health check.
//
// By default the managed task confirms a new image on its first successful
// check-in. Here the sketch confirms it instead, once its own health check
// passes. If that does not happen within the confirm timeout, the device
// goes back to the previous image.
//
// Use this when:
//   - your firmware does more than talk to SimpleOTA (sensors, actuators,
//     other servers) and all of it should work before an update is kept;
//   - the update check is the easiest part of bring-up, as on cellular.
//
// Confirm before the timeout, and don't gate the confirm on something that
// may never happen: an unconfirmed image always rolls back.

#include <WiFi.h>
#include <simpleota.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";

#define FIRMWARE_VERSION "1.0.0"

static bool wifiUp(void*) { return WiFi.status() == WL_CONNECTED; }

// Stand-in for your health check (an MQTT round trip, a sane sensor
// reading, ...). True once everything looks healthy.
static bool applicationHealthy() {
    return WiFi.status() == WL_CONNECTED;
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    simpleota_config_t cfg = {};
    cfg.token = SOTA_TOKEN;
    cfg.version_label = FIRMWARE_VERSION;
    cfg.check_interval_s = 3600;
    cfg.is_connected = wifiUp;
    // The sketch confirms, not the first check-in.
    cfg.manual_confirm = true;
    // Seconds from boot to confirm. Cover your worst-case boot-to-healthy
    // time (start-up, connecting, the first server round trip). The
    // default is 300; on cellular consider more.
    cfg.confirm_timeout_s = 120;
    if (simpleota_init(&cfg) != ESP_OK || simpleota_start() != ESP_OK) {
        Serial.println("[app] SimpleOTA did not start; see the log above");
    }
}

void loop() {
    // While this image is on trial, confirm it once the health check passes.
    if (simpleota_is_trial_boot() && applicationHealthy()) {
        // ESP_FAIL when the bootloader could not record the image as good:
        // the trial goes on, so this runs again on the next loop.
        if (simpleota_confirm() == ESP_OK) {
            Serial.println("[app] trial confirmed");
        }
    }

    delay(1000);
}
