// RollbackOTA: managed-mode with an APPLICATION-defined health gate.
//
// This sketch shows how to override the default managed-mode auto-confirm
// (which fires on the first 2xx /check/ response) so the trial only completes
// after your own application-level health check passes. If the health check
// does not pass within SIMPLEOTA_CONFIRM_TIMEOUT_S (or your override), the
// library reboots into the previous firmware partition.
//
// Typical reasons to use this pattern:
//   - Your firmware does more than talk to SimpleOTA (sensors, actuators,
//     auxiliary servers) and you want all of those proven before confirming.
//   - You ship to flaky transports (cellular) where the OTA check is the
//     easiest part of bring-up.
//
// IMPORTANT: confirmRunning() MUST be called before the timeout expires, or
// the device will roll back. Don't gate it on something that may never happen.

#include <WiFi.h>
#include <SimpleOTAClient.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";

#define FIRMWARE_VERSION "1.0.0"

SimpleOTAClient ota(SOTA_TOKEN, SimpleOTAClient::CHIP_ESP32);

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

// Pretend health probe. Replace with your actual check (MQTT round-trip,
// sensor read sanity, etc.). Returns true once everything looks healthy.
static bool applicationHealthy() {
    // ... your own logic ...
    return WiFi.status() == WL_CONNECTED;
}

static bool confirmed = false;

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);
    delay(200);
    SimpleOTAClient::setDebug(true);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    ota.setVersionLabel(FIRMWARE_VERSION);

    // Disable managed auto-confirm so the library waits for our explicit
    // confirmRunning() call instead of confirming on the first 2xx /check/.
    ota.setManagedAutoConfirm(false);

    // Give the application 120 seconds to call confirmRunning() after a
    // trial boot. Tune to the worst-case boot-to-healthy time of your
    // firmware (include OS init, transport bring-up, server handshake).
    // The default is 300 seconds; on cellular consider raising it further.
    ota.setConfirmTimeout(120);

    ota.begin(/*checkIntervalSec*/ 3600,
              /*onResult*/         nullptr,
              /*isConnected*/      wifiUp);
}

void loop() {
    // Run the application's health gate exactly once per boot. confirmRunning()
    // is a no-op when no trial is in progress, so calling it on a normal boot
    // is harmless. ota.isTrialInstall() lets you skip the work entirely when
    // there's nothing to confirm.
    // if (!confirmed && ota.isTrialInstall() && applicationHealthy()) {
    //     if (ota.confirmRunning()) {
    //         Serial.println("[app] trial confirmed");
    //     }
    //     confirmed = true;
    // }

    delay(1000);
}
