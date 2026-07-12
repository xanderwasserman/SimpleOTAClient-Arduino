// SignedOTA: managed OTA with on-device Ed25519 signature verification.
//
// Every downloaded image is verified against your project's PUBLIC signing
// key before it is marked bootable. A tampered or unapproved image is
// rejected (apply() returns OTA_SIGNATURE_FAIL), the device keeps running
// its current firmware, and the dashboard shows the device as
// failed / signature_invalid.
//
// Setup (see https://wiki.simpleota.com/guides/signed-firmware/):
//   1. Create a signing key in your project's "Firmware signing keys"
//      section (Advanced mode). Keep the PRIVATE key in your CI secret
//      store; paste the PUBLIC key below (it is not a secret).
//   2. Call setSecurityMode(SECURITY_MODE_SIGNED) so the server offers
//      this device signed builds.
//   3. Upload builds with security_mode "signed" (dashboard "Sign in
//      browser" or CI: openssl pkeyutl -sign -inkey private.pem -rawin
//      -in firmware.bin | base64).
//
// Migrating an existing basic-mode fleet? Ship THIS firmware as one last
// "basic" upload first. Devices without a pinned key apply signed offers
// checksum-only (with a serial warning), so the migration update always
// flows; every update after it is fully verified.

#include <WiFi.h>
#include <SimpleOTAClient.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

#define FIRMWARE_VERSION "1.0.0"

// Your project's PUBLIC signing key, exactly as shown in the dashboard.
// During key rotation, pin the old and new key side by side with
// ota.addSigningPublicKey("key-id", pem) (maximum of two).
// The placeholder below intentionally does NOT parse: if you flash this
// sketch without pasting your real key, setup() halts with a clear error
// instead of pinning garbage and rejecting every update.
static const char* SIGNING_PUBLIC_KEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "_PASTE_YOUR_PROJECT_PUBLIC_KEY_BODY_HERE_\n"
    "-----END PUBLIC KEY-----\n";

SimpleOTAClient ota(SOTA_TOKEN, SimpleOTAClient::CHIP_ESP32);

static bool wifiUp() {
    return WiFi.status() == WL_CONNECTED;
}

static void onOtaResult(OTAResult r) {
    if (r == OTA_SIGNATURE_FAIL) {
        // The image downloaded intact but was NOT signed with your key.
        // Investigate before anything else: check the dashboard's device
        // row (reason: signature_invalid) and your signing pipeline.
        Serial.println("[app] OTA rejected: firmware signature invalid!");
    } else if (r != OTA_SUCCESS) {
        Serial.printf("[app] OTA failed: %d\n", (int)r);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);
    delay(200);
    SimpleOTAClient::setDebug(true);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[app] Wi-Fi starting; OTA task will wait for IP.");

    ota.setVersionLabel(FIRMWARE_VERSION);

    // Advertise signed mode so the server offers signed builds, and pin the
    // public key so this device verifies them before flashing.
    //
    // Fail closed on a bad key: the PEM is a compile-time constant, so a
    // parse failure is always a developer mistake (typo, truncation) that
    // should be caught on the bench, never shipped. Without a pinned key
    // this sketch would apply signed offers checksum-only, which defeats
    // the point of this example.
    ota.setSecurityMode(SimpleOTAClient::SECURITY_MODE_SIGNED);
    if (!ota.setSigningPublicKey(SIGNING_PUBLIC_KEY_PEM)) {
        // Do not start OTA. Repeat the error so it is visible even if the
        // serial monitor attaches after boot.
        while (true) {
            Serial.println("[app] FATAL: SIGNING_PUBLIC_KEY_PEM is not a valid "
                           "Ed25519 public key. Fix the key and reflash; OTA "
                           "is disabled.");
            delay(5000);
        }
    }

    ota.begin(/*checkIntervalSec*/ 3600,
              /*onResult*/         onOtaResult,
              /*isConnected*/      wifiUp);
}

void loop() {
    // Your application code runs here; OTA happens in the background.
    delay(1000);
}
