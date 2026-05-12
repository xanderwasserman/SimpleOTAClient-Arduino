// AdvancedOTA: full manual control of the SimpleOTAClient lifecycle.
//
// Demonstrates:
//   - Custom deviceId (e.g. a serial number stored in your own flash region).
//   - boardId and hardwareRevision metadata, used by the server to target
//     deployments at specific hardware variants.
//   - setPartitionProfile(): tells the server which partition layout this
//     device uses so it only receives compatible firmware builds.
//   - setNvsSchemaVersion(): incremented when your own NVS layout changes.
//   - setLabels(): arbitrary key/value metadata for server-side targeting.
//   - setSecurityMode(): advertises TLS capability to the server.
//   - setAutoReboot(false): the application decides when to restart, and
//     uses rebootForUpdate() to emit the "reboot" event before restarting.
//   - lastOfferedVersion(): read the version label from the check() offer.
//   - Verbose logging via SimpleOTAClient::setDebug(true).

#include <WiFi.h>
#include <SimpleOTAClient.h>

#define FIRMWARE_VERSION "1.0.0"

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

// Stable per-unit identifier. Real applications might read this from a
// factory-provisioned NVS namespace, an EEPROM, or a serial number sticker
// captured at manufacturing time. The buffer must outlive the SimpleOTAClient
// instance; a static / global is the simplest pattern.
static const char* DEVICE_ID        = "sn-000123";
static const char* BOARD_ID         = "acme-widget-v2";
static const char* HARDWARE_REV     = "C";

SimpleOTAClient ota(SOTA_TOKEN,
                    SimpleOTAClient::CHIP_ESP32,
                    DEVICE_ID,
                    BOARD_ID,
                    HARDWARE_REV);

static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[app] Wi-Fi connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\n[app] Wi-Fi up: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);
    delay(200);
    SimpleOTAClient::setDebug(true);   // verbose [SimpleOTAClient] logs on Serial
    connectWiFi();

    // Confirm a prior trial install (no-op if not in trial). Call this as
    // early as your application can prove it is healthy. Here we do it
    // right after Wi-Fi succeeds; production firmware would gate it on
    // an end-to-end health probe (e.g., MQTT round-trip).
    ota.confirmRunning();

    // Tell the server which partition layout this device uses.
    // Match the scheme you selected under Tools -> Partition Scheme.
    ota.setPartitionProfile("default_4mb");

    // Advertise NVS schema version (default is 1; increment if you restructure
    // your own NVS namespaces and want server-side gating).
    ota.setNvsSchemaVersion(1);

    // Arbitrary labels for server-side deployment targeting.
    // Must be a valid JSON object literal.
    ota.setLabels("{\"site\":\"factory-A\",\"line\":\"3\"}");

    // Advertise security capability to the server.
    ota.setSecurityMode(SimpleOTAClient::SECURITY_MODE_BASIC);

    // Human-readable version label of the firmware currently running.
    // Shown on the dashboard under Reported firmware state → Version label.
    // After the first OTA, the library persists the newly-installed version
    // to NVS and reads it back automatically on subsequent boots, so this
    // call is only strictly needed to populate the label before any OTA
    // has occurred, or to override the NVS value.
    ota.setVersionLabel(FIRMWARE_VERSION);

    // Release channel, applied by the server on first registration or when
    // the device has no channel yet. Subsequent check() calls still include
    // this field, but the server only acts on it when the device is unassigned.
    ota.setChannel("stable");

    // Take manual control of the reboot.
    ota.setAutoReboot(false);

    if (ota.check()) {
        Serial.printf("[app] update offered: version=%s\n", ota.lastOfferedVersion());
        OTAResult r = ota.apply();   // does NOT reboot on success now
        switch (r) {
            case OTA_SUCCESS:
                Serial.println("[app] flash succeeded; finishing app work before reboot");
                // Application-specific shutdown: flush logs, persist state,
                // notify a parent device, etc. Then reboot when ready.
                delay(2000);
                // rebootForUpdate() emits the "reboot" lifecycle event for
                // the just-applied deployment, then calls esp_restart(). Use
                // this instead of a bare ESP.restart() when running with
                // setAutoReboot(false) so the server sees the same event
                // sequence it would on the auto-reboot path.
                ota.rebootForUpdate();
                break;
            case OTA_CHECKSUM_FAIL:
                Serial.println("[app] checksum mismatch; safe to retry later");
                break;
            case OTA_FLASH_FAIL:
                Serial.println("[app] flash failed; investigate partition table / network");
                break;
            case OTA_NO_OFFER:
                // Unreachable here because we only reach apply() after a
                // successful check(), but listed for completeness.
                break;
        }
    } else {
        Serial.println("[app] no update offered");
    }
}

void loop() {
    // Application work goes here. This sketch performs a one-shot check
    // in setup() and then returns to normal operation; schedule additional
    // checks however suits your product (timer, button press, MQTT command).
    delay(1000);
}
