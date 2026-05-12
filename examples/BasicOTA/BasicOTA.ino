// BasicOTA: minimal SimpleOTAClient integration example.
//
// Wires up Wi-Fi, then polls SimpleOTAClient once an hour. The library does NOT
// manage Wi-Fi or scheduling; that is the application's responsibility.
//
// IMPORTANT: do NOT pass your own firmware version counter as the build
// number. SimpleOTAClient assigns its own monotonic build number on upload, and
// the library persists it to NVS for you. See README.md for details.

#include <WiFi.h>
#include <SimpleOTAClient.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

// The version label of this firmware image. Used by SimpleOTAClient to report
// the currently-running firmware to the dashboard (version_label field).
// Bump this string whenever you build and upload a new release to SimpleOTA.
// After an OTA the library automatically persists the incoming version to NVS,
// so subsequent boots self-report correctly even without rebuilding; but
// declaring it here makes the firmware itself the authoritative source.
#define FIRMWARE_VERSION "1.0.0"

// Device ID auto-derived from the Wi-Fi MAC address. No setup needed.
SimpleOTAClient ota(SOTA_TOKEN, SimpleOTAClient::CHIP_ESP32);

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
    delay(200);
    SimpleOTAClient::setDebug(true);   // verbose [SimpleOTAClient] logs on Serial
    connectWiFi();

    // Report this firmware's version label to the dashboard. After the first
    // OTA the library persists the incoming version to NVS; this call ensures
    // the factory-installed image is also visible from boot zero.
    ota.setVersionLabel(FIRMWARE_VERSION);

    // TLS is verified by default against the bundled ISRG Root X1 root CA.
    // Call ota.setCACert(nullptr) to disable verification (development only).
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
    }

    // Rollback support: if the previous OTA armed a trial install, this call
    // confirms the new image is healthy. Place it where you are confident the
    // application is working (after sensors initialized, first message
    // exchanged with your service, etc.). Polling-mode users MUST call this
    // explicitly or every OTA will roll back after SIMPLEOTA_CONFIRM_TIMEOUT_S
    // (default 300s). It is a no-op when no trial is in progress.
    ota.confirmRunning();

    if (ota.check()) {
        OTAResult r = ota.apply();   // reboots on success by default
        if (r != OTA_SUCCESS) {
            Serial.println("[app] OTA failed, continuing");
        }
    }

    delay((uint32_t)SIMPLEOTA_CHECK_INTERVAL_S * 1000UL);
}
