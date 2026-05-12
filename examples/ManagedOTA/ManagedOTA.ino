// ManagedOTA: hands-off SimpleOTAClient integration using begin().
//
// The library spawns a background FreeRTOS task that periodically calls
// check() and apply() on its own. The application loop is untouched.
//
// The library is transport-agnostic: it does NOT manage Wi-Fi. The
// application brings up connectivity; the optional isConnected callback
// lets the OTA task wait for IP before each check.

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

SimpleOTAClient ota(SOTA_TOKEN, SimpleOTAClient::CHIP_ESP32);

// Probe used by the OTA task to gate each check on IP connectivity.
// Polled once a second; the task only attempts a check while it returns true.
static bool wifiUp() {
    return WiFi.status() == WL_CONNECTED;
}

// Optional result callback. With setAutoReboot(true) (the default),
// successful updates reboot before this fires, so in practice you'll only
// see failures here.
static void onOtaResult(OTAResult r) {
    if (r != OTA_SUCCESS) {
        Serial.printf("[app] OTA failed: %d\n", (int)r);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);
    delay(200);
    SimpleOTAClient::setDebug(true);   // verbose [SimpleOTAClient] logs on Serial

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[app] Wi-Fi starting; OTA task will wait for IP.");

    ota.setVersionLabel(FIRMWARE_VERSION);

    // Rollback (v0.2.0+) is enabled by default. In managed mode the library
    // auto-confirms the trial as soon as the OTA task gets its first 2xx
    // response from the server — proof that boot, connectivity, TLS, and token
    // auth are all working. If you want to gate confirmation on your own
    // application-level health check, call:
    //   ota.setManagedAutoConfirm(false);
    // and call ota.confirmRunning() yourself once your app is healthy.
    // See examples/RollbackOTA for that pattern.

    // Check every hour. The task waits for wifiUp() before each attempt
    // and reports results to onOtaResult() (only fires for non-success
    // outcomes given the default auto-reboot behaviour).
    ota.begin(/*checkIntervalSec*/ 3600,
              /*onResult*/         onOtaResult,
              /*isConnected*/      wifiUp);
}

void loop() {
    // Your application code runs here; OTA happens in the background.
    delay(1000);
}
