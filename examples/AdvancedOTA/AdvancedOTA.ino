// AdvancedOTA: polling mode with every option the device can report.
//
// Demonstrates:
//   - device_id: your own stable per-unit identifier.
//   - board_id and hardware_revision, which the server can target
//     deployments at.
//   - partition_profile: the partition layout, so the device only gets
//     builds that fit it.
//   - nvs_schema_version: raised when your own NVS layout changes.
//   - labels_json: key/value metadata for server-side targeting.
//   - security_mode and channel.
//   - disable_auto_reboot: the sketch decides when to restart, and calls
//     simpleota_reboot_for_update() so the server sees the "reboot" event.
//   - simpleota_last_offered_version(): the version label of the offer.
//   - event_cb: download progress, failure reasons and trial outcomes.
//   - Verbose logs with esp_log_level_set().

#include <WiFi.h>
#include <esp_log.h>
#include <simpleota.h>

#define FIRMWARE_VERSION "1.0.0"

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

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

// Runs on the task that drives the update; keep it short.
static void onOtaEvent(const simpleota_event_t* e, void*) {
    switch (e->id) {
    case SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS:
        Serial.printf("[app] downloaded %u of %u bytes\n", (unsigned)e->bytes_done,
                      (unsigned)e->bytes_total);
        break;
    case SIMPLEOTA_EVENT_UPDATE_FAILED:
        Serial.printf("[app] update %u failed: %s\n", (unsigned)e->build_number,
                      e->reason ? e->reason : "?");
        break;
    case SIMPLEOTA_EVENT_ROLLING_BACK:
        Serial.printf("[app] trial failed (%s), rolling back\n", e->reason ? e->reason : "?");
        break;
    default:
        break;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);

    simpleota_config_t cfg = {};
    cfg.token = SOTA_TOKEN;
    // A stable per-unit identifier, such as a serial number provisioned at
    // the factory. The default is the Wi-Fi MAC address.
    cfg.device_id = "sn-000123";
    cfg.board_id = "acme-widget-v2";
    cfg.hardware_revision = "C";
    // Match the scheme under Tools > Partition Scheme.
    cfg.partition_profile = "default_4mb";
    // 1 unless you restructure your own NVS namespaces and want the server
    // to gate builds on it.
    cfg.nvs_schema_version = 1;
    // Must be one JSON object; anything else is left out, with a warning.
    cfg.labels_json = "{\"site\":\"factory-A\",\"line\":\"3\"}";
    // The security mode the server matches builds on.
    cfg.security_mode = SIMPLEOTA_SECURITY_BASIC;
    // Applied by the server on first registration, or when the device has
    // no channel yet.
    cfg.channel = "stable";
    // Shown on the dashboard under Reported firmware state > Version label.
    cfg.version_label = FIRMWARE_VERSION;
    // Take control of the reboot.
    cfg.disable_auto_reboot = true;
    cfg.event_cb = onOtaEvent;
    if (simpleota_init(&cfg) != ESP_OK) {
        Serial.println("[app] SimpleOTA did not start; see the log above");
        return;
    }
    esp_log_level_set("simpleota", ESP_LOG_DEBUG);   // every step, on Serial

    connectWiFi();

    // Confirm a trial (nothing happens without one) as early as the sketch
    // can prove it is healthy. Here that is right after Wi-Fi; production
    // firmware would wait for an end-to-end check, such as an MQTT round
    // trip.
    simpleota_confirm();

    bool update = false;
    esp_err_t err = simpleota_check(&update);
    if (err != ESP_OK) {
        // No response or an error status: back off, the server may be down.
        Serial.printf("[app] check failed: %s\n", esp_err_to_name(err));
        return;
    }
    if (!update) {
        Serial.println("[app] no update offered");
        return;
    }
    Serial.printf("[app] update offered: version=%s\n", simpleota_last_offered_version());
    switch (simpleota_apply()) {   // disable_auto_reboot: success returns here
    case SIMPLEOTA_OK:
        Serial.println("[app] flashed; finishing app work before the reboot");
        // Flush logs, persist state, notify a parent device, and so on.
        delay(2000);
        // Posts the "reboot" event for this update, then restarts, so the
        // server sees the same events as with an automatic reboot.
        simpleota_reboot_for_update();
        break;
    case SIMPLEOTA_CHECKSUM_FAIL:
        Serial.println("[app] checksum mismatch; safe to retry later");
        break;
    case SIMPLEOTA_FLASH_FAIL:
        Serial.println("[app] update failed; see the reason in the event");
        break;
    case SIMPLEOTA_SIGNATURE_FAIL:
        Serial.println("[app] signature rejected");
        break;
    case SIMPLEOTA_UNCONFIRMED:
        Serial.println("[app] confirm the running firmware before updating");
        break;
    case SIMPLEOTA_DOWNGRADE:
        Serial.println("[app] the offered build is not newer; refused");
        break;
    case SIMPLEOTA_REBOOT_PENDING:
        Serial.println("[app] an update is already flashed; reboot to install it");
        break;
    case SIMPLEOTA_NO_OFFER:
        break;   // not after a successful simpleota_check()
    }
}

void loop() {
    // Application work goes here. This sketch checks once in setup(); check
    // again however suits your product (a timer, a button, an MQTT command).
    delay(1000);
}
