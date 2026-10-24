// SignedOTA: managed updates with on-device Ed25519 signature verification.
//
// Every downloaded image is verified against your project's PUBLIC signing
// key before it is marked bootable. A tampered or unapproved image is
// rejected (SIMPLEOTA_SIGNATURE_FAIL), the device keeps running its current
// firmware, and the dashboard shows the device as failed /
// signature_invalid.
//
// Setup (see https://wiki.simpleota.com/guides/signed-firmware/):
//   1. Create a signing key in your project's "Firmware signing keys"
//      section (Advanced mode). Keep the PRIVATE key in your CI secret
//      store; paste the PUBLIC key below (it is not a secret).
//   2. Set security_mode to SIMPLEOTA_SECURITY_SIGNED so the server offers
//      this device signed builds.
//   3. Upload builds with security_mode "signed" (dashboard "Sign in
//      browser" or CI: openssl pkeyutl -sign -inkey private.pem -rawin
//      -in firmware.bin | base64).
//
// Migrating an existing basic-mode fleet? Ship THIS firmware as one last
// "basic" upload first: a basic upload reaches devices that report basic
// and devices that report no mode. Every update after it is signed and
// verified on the device.

#include <WiFi.h>
#include <simpleota.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

#define FIRMWARE_VERSION "1.0.0"

// Your project's PUBLIC signing key, exactly as shown in the dashboard.
// The placeholder below intentionally does NOT parse: if you flash this
// sketch without pasting your real key, setup() halts with a clear error
// instead of running without verification.
static const char* SIGNING_PUBLIC_KEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "_PASTE_YOUR_PROJECT_PUBLIC_KEY_BODY_HERE_\n"
    "-----END PUBLIC KEY-----\n";

// During key rotation, pin the old and the new key side by side (at most
// two), each with its key id: {"key-2027", NEW_KEY_PEM}.
static const simpleota_signing_key_t SIGNING_KEYS[] = {
    {nullptr, SIGNING_PUBLIC_KEY_PEM},
};

static bool wifiUp(void*) {
    return WiFi.status() == WL_CONNECTED;
}

static void onOtaEvent(const simpleota_event_t* e, void*) {
    if (e->id != SIMPLEOTA_EVENT_UPDATE_FAILED) return;
    if (e->reason && strcmp(e->reason, "signature_invalid") == 0) {
        // The image downloaded intact but was NOT signed with your key.
        // Investigate before anything else: check the dashboard's device
        // row and your signing pipeline.
        Serial.println("[app] OTA rejected: firmware signature invalid!");
    } else {
        Serial.printf("[app] OTA failed: %s\n", e->reason ? e->reason : "?");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[app] Wi-Fi starting; the OTA task waits for an IP.");

    // Signed mode, so the server offers signed builds, and the pinned key,
    // so this device verifies them before flashing.
    simpleota_config_t cfg = {};
    cfg.token = SOTA_TOKEN;
    cfg.version_label = FIRMWARE_VERSION;
    cfg.security_mode = SIMPLEOTA_SECURITY_SIGNED;
    cfg.signing_keys = SIGNING_KEYS;
    cfg.num_signing_keys = sizeof(SIGNING_KEYS) / sizeof(SIGNING_KEYS[0]);
    cfg.check_interval_s = 3600;
    cfg.is_connected = wifiUp;
    cfg.event_cb = onOtaEvent;

    // Fail closed on a bad key: simpleota_init() refuses an unparseable PEM
    // rather than running unpinned. The key is a compile-time constant, so
    // this is always a typo or truncation to catch on the bench.
    if (simpleota_init(&cfg) != ESP_OK || simpleota_start() != ESP_OK) {
        // Repeat the error so it shows even if the monitor attaches late.
        while (true) {
            Serial.println("[app] FATAL: SimpleOTA did not start. Is "
                           "SIGNING_PUBLIC_KEY_PEM your project's Ed25519 "
                           "public key? OTA is disabled.");
            delay(5000);
        }
    }
}

void loop() {
    // Your application code runs here; updates happen in the background.
    delay(1000);
}
