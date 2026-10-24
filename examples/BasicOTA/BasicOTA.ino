// BasicOTA: the smallest polling integration.
//
// Brings up Wi-Fi, then once an hour confirms a running trial, checks for an
// update and applies it. The library does not manage Wi-Fi or scheduling;
// the sketch does.
//
// SimpleOTA assigns each build you upload its build number, and the library
// keeps the running one in NVS: never pass your own version counter as the
// build. A sketch flashed over USB reports build 0, so the server offers it
// its current release. See README.md.

#include <WiFi.h>
#include <esp_log.h>
#include <simpleota.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

// The version label of this image, shown on the dashboard. Bump it with each
// build you upload to SimpleOTA.
#define FIRMWARE_VERSION "1.0.0"

static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[app] Wi-Fi connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\n[app] Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);
    connectWiFi();

    // TLS is always verified, by default against the ISRG roots that
    // SimpleOTA's hosts use. For a local server, also set cfg.base_url and
    // cfg.cert_pem to your own CA.
    simpleota_config_t cfg = {};
    cfg.token = SOTA_TOKEN;
    cfg.version_label = FIRMWARE_VERSION;
    if (simpleota_init(&cfg) != ESP_OK) {
        Serial.println("[app] SimpleOTA did not start; see the log above");
    }
    esp_log_level_set("simpleota", ESP_LOG_INFO);   // log every check
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
    }

    // A new image runs as a trial, and rolls back unless it is confirmed
    // within the confirm timeout (300 s by default). Confirm where you know
    // the sketch works: here, Wi-Fi is up. Without a trial this does nothing.
    simpleota_confirm();

    bool update = false;
    if (simpleota_check(&update) == ESP_OK && update) {
        simpleota_result_t r = simpleota_apply();   // restarts on success
        Serial.printf("[app] update not installed (result %d)\n", (int)r);
    }

    delay(3600UL * 1000UL);   // check again in an hour
}
