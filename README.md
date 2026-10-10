# SimpleOTAClient

> Thin Arduino-ESP32 client library for the [SimpleOTA](https://simpleota.com) firmware update platform.

![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue)
![Framework: Arduino](https://img.shields.io/badge/framework-Arduino-teal)
![Version: 0.5.0](https://img.shields.io/badge/version-0.5.0-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

Integrates your ESP32 project with SimpleOTA in a few lines: check for an update, stream and flash it with on-the-fly SHA-256 verification, and let the library handle all the protocol bookkeeping. The application owns Wi-Fi and decides when to check; this library does the rest.

Local OTA (ArduinoOTA, espota, ElegantOTA) is for a board on your desk. SimpleOTA is managed fleet OTA for ESP32 when those devices are in the field, without pulling you into a full IoT platform.

You upload a .bin from PlatformIO, ESP-IDF, Arduino, or CI, and devices check in on their own. Eligible ones get a short-lived pre-signed URL and pull firmware straight from object storage. Rollouts can start at 5% with deterministic bucketing, so the same unit stays in the same cohort. Builds can be Ed25519-signed and verified on the device during download, before the new partition is marked bootable. After the reboot the new firmware is not yet accepted. It stays that way until `confirmRunning()` runs. A crash or a reset in that time switches the board back to the previous firmware. If the new firmware stops responding during a SimpleOTA trial, a separate clock in the chip resets the board so that switch can happen.

Signing here protects the delivery path. It is not Secure Boot. They stack if you already use Secure Boot.

[Quickstart (project, client, first update)](https://wiki.simpleota.com/getting-started/?utm_source=arduino_lib&utm_medium=readme&utm_campaign=signup_v1)

[Site](https://simpleota.com/?utm_source=arduino_lib&utm_medium=readme&utm_campaign=signup_v1)

---

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick start](#quick-start)
  - [Managed mode](#managed-mode)
  - [Polling mode](#polling-mode)
- [Build numbers](#build-numbers)
- [API reference](#api-reference)
- [OTA lifecycle](#ota-lifecycle)
- [Security](#security)
  - [Signed firmware](#signed-firmware)
- [Rollback](#rollback)
- [Configuration](#configuration)
- [Logging](#logging)
- [Partition table](#partition-table)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [Changelog](#changelog)
- [License](#license)

---

## Features

- **Two-method integration:** `check()` then `apply()` is all most projects need.
- **Streaming download with on-the-fly SHA-256:** firmware is verified before the partition is committed; no second pass, no large RAM buffer.
- **NVS build-number persistence:** the library reads and writes the SimpleOTA-assigned build number automatically.
- **Status event reporting:** reports the full update lifecycle back to the SimpleOTA backend.
- **On-device firmware signature verification (v0.4.0):** signed artifacts are verified against your pinned Ed25519 public key, streamed during download, before the image is ever marked bootable. See [Signed firmware](#signed-firmware).
- **Trial install with timeout-based rollback:** after `apply()`, the new firmware is not accepted until `confirmRunning()`. A crash, a reset, a confirm timeout, or a hang resets the board onto the previous firmware. See [Rollback](#rollback).
- **No external dependencies:** uses libraries bundled with the Arduino-ESP32 core (`HTTPClient`, `Update`, `NetworkClientSecure` / `WiFiClientSecure`, `Preferences`, `mbedtls`), plus a vendored copy of [Monocypher](https://monocypher.org) 3.1.3 for Ed25519; nothing to install.
- **Compatible with Arduino-ESP32 2.x and 3.x**.
- **Secure by default:** the bundled ISRG Root X1 root CA is used automatically; no configuration needed for production.

---

## Requirements

| Requirement | Version |
| --- | --- |
| Arduino-ESP32 core | 2.x or 3.x |
| Target hardware | Any ESP32 variant |
| Partition table | Must have two OTA app partitions (see [Partition table](#partition-table)) |
| Network | Application must establish IP connectivity before calling `check()` |

---

## Installation

**Arduino IDE (Library Manager)**

Search for `SimpleOTAClient` in *Sketch → Include Library → Manage Libraries*.

**Manual**

Clone or download this repository and copy the folder into your Arduino
`libraries/` directory:

```
~/Documents/Arduino/libraries/SimpleOTAClient/
```

**PlatformIO**

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/xanderwasserman/SimpleOTAClient-Arduino.git
```

---

## Quick start

### Managed mode

Call `begin()` from `setup()`. The library starts a FreeRTOS background task that checks for an update on a fixed interval, flashes it, and then sleeps before repeating. Your application loop is untouched.

The library is **transport-agnostic**: it does not manage Wi-Fi, Ethernet, or PPP. Bring up your network however you like, then start the OTA task.

```cpp
#include <WiFi.h>
#include <SimpleOTAClient.h>

SimpleOTAClient ota("soto_proj_xxxx", SimpleOTAClient::CHIP_ESP32);

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin("ssid", "password");
    ota.begin();   // check every hour, auto-reboot on success
}

void loop() {
    // your application code
}
```

If you can cheaply tell whether IP is up (faster startup, fewer wasted requests on a flaky link), pass an `isConnected` probe. The task polls it once a second and only attempts a check while it returns `true`:

```cpp
ota.begin(
    /*checkIntervalSec*/ 3600,
    /*onResult*/         nullptr,
    /*isConnected*/      []() { return WiFi.status() == WL_CONNECTED; });
```

For Ethernet, cellular, or any other stack, supply the equivalent probe (`ETH.linkUp()`, your modem's `isConnected()`, etc.). Without a probe, the task simply attempts each check on schedule; transport failures return `false` and the next interval handles retry.

To be notified of errors (with the default `setAutoReboot(true)`, successful updates reboot before the callback fires):

```cpp
ota.begin(3600, [](OTAResult r) {
    if (r != OTA_SUCCESS)
        Serial.printf("OTA failed: %d\n", r);
});
```

### Polling mode

Call `check()` and `apply()` from your own loop or FreeRTOS task for full control over timing and error handling:

```cpp
void loop() {
    if (ota.check()) {
        OTAResult r = ota.apply();   // reboots on success
        if (r != OTA_SUCCESS) {
            Serial.println("[app] OTA failed, continuing");
        }
    }
    delay((uint32_t)SIMPLEOTA_CHECK_INTERVAL_S * 1000UL);   // check again in 1 hour
}
```

Three runnable examples are bundled:

| Example | What it shows |
| --- | --- |
| [`examples/BasicOTA/BasicOTA.ino`](examples/BasicOTA/BasicOTA.ino) | Minimal polling integration: Wi-Fi up, `check()` + `apply()` from `loop()`. Start here. |
| [`examples/ManagedOTA/ManagedOTA.ino`](examples/ManagedOTA/ManagedOTA.ino) | Hands-off managed mode using `begin()` with an `isConnected` probe and a result callback. |
| [`examples/AdvancedOTA/AdvancedOTA.ino`](examples/AdvancedOTA/AdvancedOTA.ino) | Custom `deviceId` / `boardId` / `hardwareRevision`, `setAutoReboot(false)` with `rebootForUpdate()` for the application-driven restart, verbose logging. |

---

## Build numbers

SimpleOTA assigns its own **strictly monotonic build number** each time you upload a firmware artifact. The server uses this number (not your `APP_VERSION` string, not `__DATE__`) to decide whether a device needs an update.

**How the library manages it for you:**

| Situation | What happens |
| --- | --- |
| Factory-fresh device (empty NVS) | Library sends `current_build_number: 0` |
| After a successful `apply()` | Library writes the build number from the check response into NVS (`simpleota` / `sota_build`, uint32) |
| Every subsequent `check()` | Library reads `sota_build` from NVS and sends it |

**Do not write to the `simpleota`/`sota_build` NVS key from your own code.** Overwriting it with any value other than one the server gave you will break the server's comparison: the device will either never receive another update, or be offered a build it already has.

---

## API reference

### Constructor

```cpp
SimpleOTAClient(const char* token,
          const char* chipFamily,
          const char* deviceId         = nullptr,
          const char* boardId          = nullptr,
          const char* hardwareRevision = nullptr);
```

> **Note:** `deviceId` must point to a buffer that outlives the `SimpleOTAClient` object. A `static char` array formatted in `setup()` (see [Quick start](#quick-start)) is the standard pattern.

| Parameter | Description |
| --- | --- |
| `token` | Project token or device token. Sent as `Authorization: Bearer <token>`. A **project token** (`soto_proj_...`) authenticates any device in the project; the server uses `deviceId` to identify the specific device. A **device token** authenticates a single pre-registered device; the server resolves it directly and ignores `deviceId` from the payload. |
| `chipFamily` | Espressif chip variant. Use the provided constants: `SimpleOTAClient::CHIP_ESP32`, `CHIP_ESP32S2`, `CHIP_ESP32S3`, `CHIP_ESP32C3`, `CHIP_ESP32C6`, `CHIP_ESP32H2`. A raw string literal is also accepted for unlisted variants. |
| `deviceId` | Optional. Stable per-unit identifier. When `nullptr` (default), the library automatically uses the Wi-Fi MAC address formatted as `"aa:bb:cc:dd:ee:ff"`. Pass a custom string to override. |
| `boardId` | Optional. Omitted from requests when `nullptr`. |
| `hardwareRevision` | Optional. Omitted from requests when `nullptr`. |

String arguments are not copied; they must remain valid for the lifetime of the `SimpleOTAClient` object. String literals and `static` buffers are fine. When `deviceId` is `nullptr`, the library uses its own internal buffer.

---

### `bool check()`

POSTs to `/api/v1/ota/check/` with the device identity and the current build number from NVS.

Returns `true` if the server offers an update and stores the offer details (`url`, `checksum`, `build_number`, `deployment_id`) on the instance for `apply()` to consume.

Returns `false` on no-update, malformed response, or transport failure.

Per the SimpleOTA protocol, **all business outcomes (no update, device over limit, project inactive) return HTTP 200**. The library never retries a 200 response, so calling `check()` in a polling loop is safe.

---

### `OTAResult apply()`

Executes the offer stored by the most recent successful `check()`.

| Return value | Meaning |
| --- | --- |
| `OTA_SUCCESS` | Firmware flashed and build number persisted. Device reboots before this is observed when `setAutoReboot(true)` (the default); with `setAutoReboot(false)` the caller is responsible for rebooting. |
| `OTA_CHECKSUM_FAIL` | Downloaded payload did not match the expected SHA-256. Partition was **not** committed. Safe to retry. |
| `OTA_FLASH_FAIL` | Network or flash write error. Partition was not committed. |
| `OTA_NO_OFFER` | `check()` had not been called or returned `false`. |
| `OTA_SIGNATURE_FAIL` | Ed25519 signature verification failed for a signed artifact. The partition was **not** committed. A `failed` event with reason `signature_invalid` was reported. Treat this as a security signal. See [Signed firmware](#signed-firmware). |
| `OTA_UNCONFIRMED` | The running firmware has not been confirmed yet. `apply()` does not flash the new image and does not tell the server the update failed. Call `confirmRunning()`, then call `apply()` again. The offer is kept. |

> **Note on `validated`:** this event is reported after `Update.end()` succeeds (meaning "the image was flashed cleanly"), not after a successful boot. A subsequent `reboot` event is emitted immediately before `esp_restart()` on the auto-reboot path; a `confirmed` event is emitted on the first 2xx `/check/` after a successful trial confirmation (see [Rollback](#rollback)).

---

### `bool report(const char* event, const char* reason = nullptr)`

Manually posts a single status event to `/api/v1/ota/status/`. Useful for custom lifecycle control flows. Returns `true` on HTTP 2xx with `"accepted": true`.

Requires a deployment context populated by a successful `check()`. The context is retained through `apply()` (so post-apply `report()` calls work) and is cleared on the next `check()` invocation. Outside that window, `report()` returns `false` without sending anything.

Full event vocabulary defined by the API:

```
offered  download_started  downloaded  flashed  validated  reboot  confirmed  failed  rolled_back
```

Events emitted automatically by `apply()`: `download_started`, `downloaded`, `flashed`, `validated`, `failed`, and (on the auto-reboot path) `reboot`. The `confirmed` event is emitted by the library on the first 2xx `/check/` after a successful trial confirmation (see [Rollback](#rollback)); `rolled_back` is emitted on the first 2xx `/check/` after a trial-timeout rollback.

---

### `void setCACert(const char* pemRootCA)`

Overrides the CA certificate used for HTTPS verification. By default the library uses the bundled `kSimpleOtaRootCA` (ISRG Root X1), which covers both the SimpleOTA API and its firmware storage. Call this only if you need to pin a different certificate.

---

### `void setAutoReboot(bool enabled)`

Default: `true`. When `false`, `apply()` returns `OTA_SUCCESS` without calling `esp_restart()`, giving the application control over when the reboot happens. The NVS build number write and `validated` event still occur before the function returns. The library does **not** emit a `reboot` event in this mode; use [`rebootForUpdate()`](#void-rebootforupdate) when the application is ready to restart so the server sees the same event sequence as the auto-reboot path.

---

### `void rebootForUpdate()`

Convenience for applications running with `setAutoReboot(false)`. Emits the `reboot` lifecycle event for the just-applied deployment, then calls `esp_restart()`. Does not return. Equivalent to:

```cpp
ota.report("reboot");
ESP.restart();
```

Must be called in the post-apply window (between a successful `apply()` and the next `check()`); outside that window the status POST is silently skipped and the device still reboots.

---

### `void setPartitionProfile(const char* profile)`

Reports the device's partition layout to the server (e.g. `"default_4mb"`, `"minimal_spiffs_4mb"`). Required when your SimpleOTA artifacts are tagged with a `partition_profile`; the server uses this field to filter hardware-compatible builds and will not offer a build whose partition profile doesn't match. Pass `nullptr` (default) to omit the field.

The value should match the scheme you selected in the Arduino IDE under *Tools → Partition Scheme*, or the `board_build.partitions` value in your `platformio.ini`.

---

### `void setNvsSchemaVersion(uint8_t version)`

Default: `1`. Increment this when you restructure your own NVS namespace layout and want the server to gate builds that require a specific schema version. The library always sends this field explicitly; the server assumes `1` when it is absent.

---

### `void setLabels(const char* jsonObject)`

Arbitrary key/value metadata sent to the server for deployment targeting. Must be a valid JSON object literal string, e.g. `"{\"site\":\"factory-A\"}"`. The string is embedded verbatim in the request body; no escaping or validation is performed by the library. Pass `nullptr` (default) to omit the field.

---

### `void setSecurityMode(const char* mode)`

Advertises the device's security capability to the server. Used for server-side compatibility filtering: a mismatch between the device's declared mode and the artifact's required mode blocks the update. Pass `nullptr` (default) to omit the field.

Use the provided constants to avoid typos:

| Constant | Value | Description |
|---|---|---|
| `SimpleOTAClient::SECURITY_MODE_BASIC` | `"basic"` | HTTPS + SHA-256 checksum verification. Default. |
| `SimpleOTAClient::SECURITY_MODE_TOKEN` | `"token"` | Per-device token authentication. Partially implemented on the server; advisory today. |
| `SimpleOTAClient::SECURITY_MODE_SIGNED` | `"signed"` | On-device Ed25519 firmware signature verification. Pin your key with `setSigningPublicKey()`; see [Signed firmware](#signed-firmware). |

A raw string literal is also accepted for forward compatibility.

---

### `bool setSigningPublicKey(const char* pem)`

Pins the project's Ed25519 **public** signing key (replacing any previously pinned keys) and enables on-device signature verification for signed artifacts. Takes the PEM exactly as issued by the SimpleOTA dashboard or API; the raw key bytes are copied, so the string need not outlive the call. Returns `false` if the PEM is not a valid Ed25519 public key.

See [Signed firmware](#signed-firmware) for the full behavior contract.

---

### `bool addSigningPublicKey(const char* keyId, const char* pem)`

Pins an additional key (maximum two) for key-rotation windows. When the server's offer names a `signing_key_id` matching one of the given `keyId`s, only that key is tried; otherwise all pinned keys are tried and the signature is accepted if it verifies under any of them. Returns `false` when two keys are already pinned or the PEM is invalid.

---

### `void setVersionLabel(const char* label)`

Sets the human-readable version label of the firmware **currently running** (e.g. `"1.4.1"`). Sent as `version_label` and shown on the dashboard under *Reported firmware state → Version label*. Display only; not used for compatibility checks.

When `nullptr` (default), the library automatically reads the value persisted in NVS by the last successful `apply()`, so post-OTA boots report the correct label without any code change in the new firmware image. You only need this call to provide the initial label before any OTA has occurred, or to override the NVS value.

---

### `void setChannel(const char* channel)`

Sets the release channel this device subscribes to (e.g. `"stable"`, `"beta"`). Only applied by the server on first registration or when the device has no channel yet. Pass `nullptr` (default) to omit the field.

---

### `const char* lastOfferedVersion()`

Returns the version label string from the last update offered by `check()` (e.g. `"1.4.2"`). Valid between a successful `check()` and the `apply()` call that consumes the offer. Returns an empty string before any offer has been received and after `apply()` clears the offer.

---

### `static void setDebug(bool enabled)`

Enables or disables verbose `[SimpleOTAClient]` logging on `Serial` at runtime. See [Logging](#logging).

---

### `void setRollbackEnabled(bool enabled)`

Enables or disables the library-managed trial-install / rollback machinery. Default: `true`. See [Rollback](#rollback). When `false`, `apply()` skips the pre-OTA snapshot and `confirmRunning()` always returns `false` (no trial is ever armed). The `confirmed` event is still emitted on the first 2xx `/check/` after a reboot, via the NVS-backed deferred path.

---

### `void setManagedAutoConfirm(bool enabled)`

In managed mode (`begin()`), controls whether the library automatically calls `confirmRunning()` on the first 2xx response from `/api/v1/ota/check/` after a trial boot. Default: `true`. Set to `false` to require an explicit `confirmRunning()` call from your application code instead. No effect in polling mode (where you always call `confirmRunning()` yourself).

---

### `void setConfirmTimeout(uint32_t seconds)`

Sets the per-instance trial-install confirm timeout in seconds. The default is `SIMPLEOTA_CONFIRM_TIMEOUT_S` (300). Values outside 10 to 86400 seconds are brought inside that range. Calling this while a trial is running restarts the confirm timer. It restarts the chip watchdog to the same deadline. Calling it after `confirmRunning()` does not start the watchdog again. Call it at the top of `setup()` before a long network attach. See [Rollback](#rollback).

---

### `bool confirmRunning()`

Confirms that the currently-running firmware is healthy. This stops the confirm timer. It stops the chip watchdog. It tells the bootloader the firmware is good, then clears the saved trial note. It queues a `confirmed` status event for the next successful `/check/` round-trip. It returns `true` when a trial was in progress and is now confirmed. It returns `false` when no trial was in progress, which is a normal boot and a safe no-op. Call it before `esp_deep_sleep_start()` during a trial.

Call from your application code once you are satisfied the new firmware is working. In polling mode this is required after every successful OTA; in managed mode it is only required if you have called `setManagedAutoConfirm(false)`.

---

### `bool isTrialInstall()`

Returns `true` while a trial install is in progress (i.e. the current boot is the first boot of a new firmware image and `confirmRunning()` has not yet been called). Use this to avoid expensive health-probe code on normal boots.

---

### `void begin(...)`

Starts a FreeRTOS background task (stack: 8 KB, priority: 1) that runs the following loop:

1. **If `initialDelayMs` > 0**, sleep for that many milliseconds (one-shot, at task start only). Use this to stagger the first HTTPS request relative to other TLS sessions starting concurrently at boot (e.g. MQTT). Default: `0`.
2. If `isConnected` was supplied, wait until it returns `true` (polled every second). Otherwise skip this step.
3. **If a trial install is in progress**, run a short-retry inner loop: retry `/check/` every `SIMPLEOTA_TRIAL_RETRY_INTERVAL_S` seconds (default 10 s) until the server responds with 2xx or the confirm timeout expires. With `setManagedAutoConfirm(true)` (default), call `confirmRunning()` on the first 2xx and continue; the `confirmed` event fires on that same `/check/` response. Skip this step on a normal (non-trial) boot.
4. Call `check()`. If an update is available, call `apply()`.
5. If `onResult` is set and `apply()` returned without rebooting, invoke it with the `OTAResult`.
6. Sleep for `checkIntervalSec` seconds, then repeat.

The library is transport-agnostic and does not import any networking stack. The application is responsible for bringing up Wi-Fi, Ethernet, PPP, or whatever connectivity it uses. The `isConnected` callback is purely an optimisation; without it, transport failures simply return `false` from `check()` and are retried on the next interval.

Safe to call once from `setup()`. Calling `begin()` while a task is already running is a no-op.

> **Note:** with the default `setAutoReboot(true)`, a successful flash triggers `esp_restart()` before `onResult` is called. Use `setAutoReboot(false)` if you want the callback to fire on success as well.

---

### `void end()`

Deletes the background task started by `begin()`. Do not call while a firmware flash is in progress.

---

### `static const char* SimpleOTAClient::kSimpleOtaRootCA`

Bundled root CA PEM (ISRG Root X1 / Let's Encrypt), used by default for all HTTPS connections. Both the SimpleOTA API and its firmware storage chain to this root, so no additional configuration is needed.

---

## OTA lifecycle

The sequence of events reported to the server during a successful `apply()`:

```
check()  →  download_started  →  downloaded  →  flashed  →  validated  →  reboot  →  [esp_restart()]  →  confirmed
```

The terminal `confirmed` event fires on the first 2xx `/check/` after the new image boots successfully and `confirmRunning()` runs (managed mode does this automatically by default; polling mode requires an explicit call). If the trial times out instead, the library rolls back and a `rolled_back` event takes the place of `confirmed`.

On failure at any stage, a `failed` event is reported with a short reason token before the method returns:

| Reason token | Stage |
| --- | --- |
| `insecure_url` | URL did not begin with `https://` |
| `https_begin_failed` | Could not open HTTPS connection to download URL |
| `http_status_<N>` | Server returned non-200 for the firmware GET |
| `update_begin_failed` | `Update.begin()` failed, likely a partition issue |
| `update_write_failed` | Flash write error during streaming |
| `stream_error` | Socket error mid-download |
| `download_stalled` | No bytes received within `SIMPLEOTA_TIMEOUT_MS` |
| `short_read` | Stream closed before `Content-Length` bytes arrived |
| `checksum_mismatch` | SHA-256 of received bytes did not match server's value |
| `update_end_failed` | `Update.end()` failed after streaming completed |

If [rollback](#rollback) is enabled and a trial install is not confirmed within the timeout, the library reboots into the previous partition. Once the next `check()` succeeds, it sends a separate `rolled_back` event. The reason is `confirm_timeout` when the confirm timer asked for the switch. A crash, a reset, or the chip watchdog uses the reset reason instead.

---

## Security

> **Read this section before deploying to a production fleet.**

The integrity guarantee of this library rests on the SHA-256 `checksum` field from the `/check/` response. That field travels over the same TLS channel as the firmware `url`. **If that TLS channel is not verified, an on-path attacker can supply any `(url, checksum)` pair (including for a malicious firmware image), and the device will flash it without complaint.**

### Default mode: secure

The library defaults to verifying TLS using the bundled ISRG Root X1 root CA (`kSimpleOtaRootCA`). This covers both the SimpleOTA API and the firmware download endpoint. No additional configuration is needed for production.

### Disabling TLS verification

Passing `nullptr` to `setCACert()` switches to `WiFiClientSecure::setInsecure()`, disabling certificate verification entirely. The library will emit a one-shot `Serial` warning whenever a request is made in this mode:

```
[SimpleOTAClient] WARNING: TLS verification disabled. Call setCACert(SimpleOTAClient::kSimpleOtaRootCA) for production.
```

This mode exists only to simplify local development. **Do not use it in production.**

### Other hardening built into the library

- Download URLs that don't begin with `https://` are rejected before any connection is made.
- Project tokens containing CR/LF or control characters are rejected to prevent HTTP header injection.
- The library never retries a received HTTP response; only transport-level failures trigger a retry. This prevents fleet-scale retry storms.

### Signed firmware

Since v0.4.0 the library verifies **Ed25519 firmware signatures on the device**, upgrading the integrity guarantee from "trust the TLS channel" to "trust only images approved by the holder of the project's private signing key". Even a fully compromised delivery path (server, storage, TLS) cannot make the device flash an unapproved image.

Setup ([full guide](https://wiki.simpleota.com/guides/signed-firmware/)):

```cpp
ota.setSecurityMode(SimpleOTAClient::SECURITY_MODE_SIGNED);
ota.setSigningPublicKey(
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEA...your project public key...\n"
    "-----END PUBLIC KEY-----\n");
```

The signature (delivered in the `/check/` response for signed artifacts) is verified incrementally over the exact downloaded bytes, in the same streaming pass as the SHA-256 checksum, **before** `Update.end()` marks the new image bootable. No extra RAM buffer, no second pass over flash.

A device is **enforcing** once it both reports signed mode
(`setSecurityMode(SECURITY_MODE_SIGNED)`) and has a key pinned. For an
enforcing device the decision is made from device state, not from what the
offer claims, so a compromised server or MITM cannot downgrade it by
stripping the signature fields:

| Offer | Enforcing? | Result |
| --- | --- | --- |
| Signed, valid signature | yes | Verified and flashed. |
| Signed, tampered signature | yes | Rejected before the image is bootable. `apply()` returns `OTA_SIGNATURE_FAIL`; a `failed` event with reason `signature_invalid` is reported (shown on the dashboard device row). Device keeps its current firmware. |
| Signature missing, or offer arrives **without** signed fields | yes | Rejected before the download even starts (same `OTA_SIGNATURE_FAIL` / `signature_invalid`). This is the anti-downgrade guarantee. |
| Signed | no key pinned | Applied with checksum verification only, plus a `Serial` warning on each such apply. Keeps fleet migration safe (below). |
| Not signed | not enforcing | Identical to previous releases. |

#### How the server matches modes

The server treats `security_mode` as a compatibility dimension: an artifact is only offered to a device when the two values are **exactly equal, or either side is blank**. A device that never calls `setSecurityMode()` reports nothing and matches every artifact. The rule is symmetric:

| Device reports | `basic` artifact | `signed` artifact |
| --- | --- | --- |
| *(nothing)* | offered | offered |
| `basic` | offered | **not offered** |
| `signed` | **not offered** | offered |

A blocked device does not error: it receives a normal `no_compatible_build` response and silently stays on its current build.

#### Migrating a fleet from `basic` to `signed`

Keys are compiled into the firmware, and the device's reported mode is changed by the firmware it runs, never by the mode of the artifact that delivered it. So:

1. Ship one final **`basic`** upload whose code adds `setSigningPublicKey(...)` and `setSecurityMode(SECURITY_MODE_SIGNED)`. A `basic` artifact reaches devices reporting `basic` *and* devices that never reported a mode, so the whole fleet gets it regardless of history.
2. Devices running it report `security_mode: "signed"` and verify every subsequent update on-device.
3. From then on, **upload only signed artifacts**: per the matrix above, a `signed` device is never offered a `basic` artifact, so there is no quiet fallback to unsigned releases.

Do not upload the migration build itself as `signed`: devices explicitly reporting `basic` will never be offered it, and devices that would accept it cannot verify it yet anyway.

> **Watch for stragglers.** After migrating, check the project device list for devices still reporting `basic` (stuck on an old build): they missed the migration artifact and will receive nothing once you publish only signed uploads.

**Key rotation:** create the new key server-side, ship a firmware update pinning **both** keys via `addSigningPublicKey()`, start signing with the new key, then revoke the old key server-side and drop it from the next firmware.

#### Scope: this is not Secure Boot

Verification runs inside the application, so the trust anchor is the firmware the device is already running. That protects the **OTA delivery path** (compromised account, leaked upload token, tampered storage, MITM); it does **not** protect the device itself. An attacker with physical access can still reflash arbitrary firmware over UART/USB via the ESP32's ROM download mode, and firmware that is already compromised will not verify itself. The hardware answer to those threats is [ESP32 Secure Boot](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/secure-boot-v2.html) (plus Flash Encryption), which is complementary: SimpleOTA signing protects the supply chain to the device, Secure Boot protects the device itself. Note that stock Arduino-ESP32 cores ship precompiled bootloaders built **without** Secure Boot, so enabling it requires building under ESP-IDF; on the Arduino path, OTA-level signing is the strongest firmware-integrity protection available.

Ed25519 verification uses a vendored, unmodified copy of [Monocypher](https://monocypher.org) 3.1.3 (dual-licensed CC0-1.0 / BSD-2-Clause).

---

## Rollback

Since v0.2.0 a new firmware can be tried and then abandoned if it does not settle. When `apply()` succeeds, the library saves the partition address and the stored identity of the firmware it is replacing. It then reboots into the new firmware.

On a stock Arduino-ESP32 core the bootloader can switch back to the previous firmware when the new one is never accepted. This library holds that open only when it wrote its own trial note for this boot. That note is `sota_trial` set to 1, together with the previous partition address. `initArduino()` then leaves that SimpleOTA firmware unconfirmed through `setup()`. An image written by ArduinoOTA or HTTPUpdate is accepted by the Arduino core on that boot, because this library did not write a trial note for it.

`confirmRunning()` tells the bootloader the firmware is good. It stops the chip watchdog. It clears the trial note. A crash or a reset before that call switches back to the previous firmware. If power is lost after the bootloader has been told the firmware is good, and before the trial note is cleared, the next boot treats that note as already confirmed. The timer is not started. The firmware is not switched back.

If `confirmRunning()` is not called before the confirm time runs out, the library asks the bootloader to switch back. That call reboots the board. The default confirm time is 300 seconds (`SIMPLEOTA_CONFIRM_TIMEOUT_S`). If the bootloader call returns, the library switches the saved partition itself and restarts.

On a SimpleOTA trial the library starts a watchdog that uses the chip's slow clock. It starts that watchdog from `verifyRollbackLater()`, before `setup()`. The wait is the confirm time plus a margin. The margin is the larger of 60 seconds and 10 percent of the confirm time. A hang before `begin()`, a hang in `setup()`, or a hang in `loop()` resets the chip while that watchdog is running. The bootloader then switches back to the previous firmware. This covers a hang the FreeRTOS timer task cannot interrupt. A hang inside a global constructor runs before `verifyRollbackLater()` and is not covered.

The chip watchdog can wait at most about 8 hours at the usual slow clock. On the ESP32-S2 it can wait about 13 hours. A longer `setConfirmTimeout()` still starts the software timer at the time you asked for. The chip watchdog fires at its maximum.

### What you must do

Call `confirmRunning()` before the first `esp_deep_sleep_start()` of a trial boot. Light sleep pauses the chip watchdog. Deep sleep does not. Waking from deep sleep boots the chip again. That boot starts the full confirm time over. A device whose awake time always stays under the limit never times out. `apply()` then keeps returning `OTA_UNCONFIRMED`. The first reset that is not a deep-sleep wake switches back to the previous firmware, even when that firmware was healthy. A stock bootloader keeps an unconfirmed image across deep sleep only when skip-validate-in-deep-sleep is enabled. A build that omits that setting warns at compile time.

Call `setConfirmTimeout()` at the top of `setup()`, before a long network attach. While a trial is running, that call restarts the confirm timer. It restarts the chip watchdog to the same deadline. A bring-up that runs longer than the compiled-in confirm time plus its margin, before that call, resets the chip.

Call `confirmRunning()` within the confirm time after boot. A polling sketch that first calls the library hours after boot used to stay on the new firmware. The chip watchdog now resets that board. The bootloader then switches back. The examples BasicOTA and AdvancedOTA call `confirmRunning()` from `setup()`.

Use one `SimpleOTAClient` object in the firmware. The trial phase is shared by the whole program.

### Opting out

These flags have to be real compiler defines. A `#define` in the sketch does not reach the library source file. A sketch that also defines `verifyRollbackLater()` fails to link, because the library already defines that function.

`-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` skips the hook and the bootloader mark calls. Trial rollback then uses only the saved record.

`-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG` leaves the bootloader switch in place. It does not start the chip watchdog, so a hang is not reset by this library.

With Arduino-ESP32 core 2.0.17 or later, put the flag in `build_opt.h` next to the sketch, in any Arduino IDE version, one flag per line:

```
-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK
```

The same flag can be passed from arduino-cli or from PlatformIO.

```
arduino-cli compile --fqbn esp32:esp32:esp32 --build-property compiler.cpp.extra_flags=-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK
```

In PlatformIO, add this to `platformio.ini`:

```
build_flags = -DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK
```

Every chip the Arduino-ESP32 core supports has this watchdog behind the same interface. That list is the ESP32, S2, S3, C2, C3, C5, C6, H2, and P4. A target whose build has no `hal/wdt_hal.h` skips the arm. The library prints that a hang during the trial is not reset by the library. A crash or a reset still switches back through the bootloader. See [Limitations](#limitations).

### Polling mode

You must call `confirmRunning()` explicitly. The natural place is at the top of `loop()`, *after* your application has proven it can do real work:

```cpp
void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    ota.confirmRunning();   // no-op when not in trial; safe to call every iteration
    // ... your app code ...
    if (ota.check()) ota.apply();
    delay(SIMPLEOTA_CHECK_INTERVAL_S * 1000UL);
}
```

If you never call `confirmRunning()`, every successful OTA will roll back.

### Managed mode (default)

`begin()` runs the trial loop on its own. By default it auto-confirms the trial as soon as the OTA task gets its first 2xx response from `/api/v1/ota/check/`, which transitively proves boot, connectivity, TLS, token auth, and server reachability. No app code change is needed.

To gate confirmation on your own application-level health check instead:

```cpp
ota.setManagedAutoConfirm(false);
ota.setConfirmTimeout(120);   // give your app 120 s to confirm
ota.begin(...);
// ...
if (ota.isTrialInstall() && applicationHealthy()) {
    ota.confirmRunning();
}
```

See `examples/RollbackOTA` for the full pattern.

### Cellular and other high-latency transports

The default 300 second timeout assumes Wi-Fi. For cellular or other transports, attaching and then registering and then reaching the API can take several minutes. Call `setConfirmTimeout()` at the top of `setup()`, before that attach. Values outside 10 to 86400 seconds are brought inside that range. `setConfirmTimeout(7200)` is two hours and does not wrap the timer. A bring-up that runs longer than the compiled-in confirm time plus the margin, before that call, resets the chip. The bootloader then switches back. The chip watchdog can wait at most about 8 hours at the usual slow clock, or about 13 hours on the ESP32-S2. A longer timeout still starts the software timer at the time you asked for. The chip watchdog fires at its maximum.

### Server interaction

When the library rolls back, the previous boot's snapshot is restored to the partition and to NVS (build number, hash, version label). On the next boot, after `check()` makes its first successful round-trip, the library sends a `rolled_back` status event with the failed deployment's `deployment_id` and `build_number`. The reason is `confirm_timeout` when this library's confirm timer asked for the switch. After a crash, a reset, or the chip watchdog, the reason is taken from that reset: `panic`, `int_wdt`, `task_wdt`, `watchdog`, `brownout`, `power_on`, `deep_sleep`, or `reset`. The event is retried on every subsequent `check()` until the server accepts it.

On a successful trial the device boots the new firmware. `confirmRunning()` runs. The library queues a `confirmed` status event. It is sent on the first 2xx from `/check/` after `confirmRunning()` has run. It is retried on every subsequent `check()` until the server accepts it. Both `confirmed` and `rolled_back` are saved in NVS so they survive a power cycle between the event and the next network round-trip.

### Disabling rollback

If you do not want the rollback machinery at all:

```cpp
ota.setRollbackEnabled(false);
```

This skips the snapshot in `apply()` and the trial-boot arming entirely. Any residual trial state left in NVS from a previous boot with rollback enabled is cleaned up on the next boot. An unconfirmed image from that leftover trial is marked valid, so the bootloader does not keep holding it. No manual NVS clearing is needed. The compile-time flags are described in [Opting out](#opting-out).

---

## Configuration

Override these with a `-D` compiler flag. A `#define` in the sketch does not change a value the library `.cpp` compiled in. See [Opting out](#opting-out) for `build_opt.h`, arduino-cli, and PlatformIO.

| Define | Default | Description |
| --- | --- | --- |
| `SIMPLEOTA_TIMEOUT_MS` | `15000` | An HTTP request or a stalled download is abandoned after this many milliseconds. |
| `SIMPLEOTA_CHECK_INTERVAL_S` | `3600` | This is the default check interval in seconds used by `begin()`. It is also a constant for manual scheduling in polling mode. |
| `SIMPLEOTA_CONFIRM_TIMEOUT_S` | `300` | This is the default trial-install confirm timeout in seconds. See [Rollback](#rollback). `setConfirmTimeout()` overrides it for one instance. The library `.cpp` reads this value, so override it with a `-D` flag. |
| `SIMPLEOTA_TRIAL_WDT_MARGIN_S` | `60` | This is the smallest extra time, in seconds, added when the chip watchdog is started. If 10 percent of the confirm time is longer, that longer value is used instead. Override it with `-DSIMPLEOTA_TRIAL_WDT_MARGIN_S=N`. |
| `SIMPLEOTA_TRIAL_RETRY_INTERVAL_S` | `10` | The managed task retries `/check/` this often, in seconds, during a trial install. |
| `SIMPLEOTA_DEBUG` | `0` | This is the compile-time default for verbose `[SimpleOTAClient]` logging on `Serial`. Prefer `SimpleOTAClient::setDebug(true)` from the sketch. See [Logging](#logging). |
| `SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` | unset | Pass this as a `-D` build flag. The library does not define `verifyRollbackLater()` and does not call the bootloader mark or rollback functions. Trial rollback stays on the saved-record path. See [Opting out](#opting-out). |
| `SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` | unset | Pass this as a `-D` build flag. The bootloader can still switch back after a crash or a reset. The chip watchdog is not started, so a hang during the trial is not reset by this library. |

**Retry policy:** `check()` and status posts retry once after a 2-second delay on transport failure (HTTP code ≤ 0). They never retry on any received HTTP response. The firmware download in `apply()` does not retry; a failure returns `OTA_FLASH_FAIL` and the next `check()` cycle can try again.

---

## Logging

Enable verbose logging at runtime from your sketch:

```cpp
void setup() {
    Serial.begin(115200);
    SimpleOTAClient::setDebug(true);
    // ...
}
```

All messages are prefixed with `[SimpleOTAClient]` and written via `Serial.printf`. Ensure `Serial.begin()` has been called before any library method is invoked.

The runtime toggle is the recommended path. A compile-time default also exists: build with `-DSIMPLEOTA_DEBUG=1` to make logging on by default. (A `#define SIMPLEOTA_DEBUG 1` placed in your sketch will **not** work, because the library's `.cpp` is compiled in a separate translation unit that doesn't see sketch-level defines. Use `setDebug(true)` or a real build flag.)

The insecure-TLS warning is always emitted on `Serial`, regardless of this setting.

---

## Partition table

The device must be flashed with a partition table that includes at least two OTA application partitions. The Arduino IDE's built-in options that work:

- *Default 4MB with spiffs*
- *Minimal SPIFFS (Large APPS with OTA)*

If `Update.begin()` fails at runtime, this is the most likely cause. Verify your partition table in the Arduino IDE under *Tools → Partition Scheme*, or in your `platformio.ini` with `board_build.partitions`.

---

## Limitations

| Limitation | Detail |
| --- | --- |
| Previous partition not saved | If `apply()` cannot save the previous partition, the next boot does not hold the new firmware open. The Arduino core accepts that image. |
| Hang before `verifyRollbackLater()` | A hang in a global constructor is not covered. The library has not started the chip watchdog yet, so that hang is not reset. The bootloader does not switch back. Crashes and resets after the library's startup hook switch back when this boot is a SimpleOTA trial. Hangs switch back once the chip watchdog has been started. Building with `-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG`, or building for a target with no `hal/wdt_hal.h`, leaves hangs unprotected. The library prints that. The bootloader still switches back after a crash or a reset unless `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` is set. A sketch that uses the chip watchdog itself should start it from `setup()`. A SimpleOTA trial uses that same watchdog until `confirmRunning()`. |
| No automatic `reboot` event when `setAutoReboot(false)` | The library only emits `reboot` on the auto-reboot path it controls. Applications that drive their own restart should call `rebootForUpdate()` (which emits the event then calls `esp_restart()`) instead of `ESP.restart()` directly; see AdvancedOTA. |
| `report()` requires a deployment context | The method needs a `deployment_id`, which only exists after a successful `check()`. The deployment context is retained through `apply()` so post-apply events (e.g. `"reboot"`) work, but `report()` returns `false` before any `check()` has succeeded. |
| Application owns connectivity | The library is transport-agnostic and does not manage Wi-Fi, Ethernet, PPP, or reconnects. Establish a working IP connection before calling any library method, or supply an `isConnected` probe to `begin()`. |
| TLS uses bundled root CA | The bundled ISRG Root X1 cert covers current SimpleOTA infrastructure. If the platform migrates storage providers to one using a different root, a library update will be required. |

---

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `check()` always returns `false` | Bad token, or no active deployment targeting this device. Enable `SIMPLEOTA_DEBUG` and check the HTTP response code in the logs. |
| `Update.begin failed` | Partition table has no OTA slot, or the OTA partition is too small for the firmware being applied. |
| `OTA_CHECKSUM_FAIL` | The firmware object was replaced or corrupted on the server between the `/check/` response and the download. |
| Device is offered the same build repeatedly | The `simpleota`/`sota_build` NVS key was cleared or overwritten. See [Build numbers](#build-numbers). A `current_build_number` of `0` always matches a pending deployment, so a wiped NVS will keep re-offering the same build. |
| Download stalls or times out | Weak Wi-Fi signal, or `SIMPLEOTA_TIMEOUT_MS` is too short for the link speed and firmware size. Increase the timeout or improve signal quality. |
| `[SimpleOTAClient] WARNING: TLS verification disabled` | `setCACert(nullptr)` was called. Only use insecure mode during local development. See [Security](#security). |

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md). Version 0.5.0 holds a new SimpleOTA firmware unconfirmed until `confirmRunning()` runs, or until managed mode confirms it. A polling sketch that used to call the library hours after boot now has to confirm inside the confirm time. Otherwise the chip watchdog resets the board.

---

## License

MIT License. See [LICENSE](LICENSE) for the full text.
