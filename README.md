# SimpleOTAClient

> Thin Arduino-ESP32 client library for the [SimpleOTA](https://simpleota.com) firmware update platform.

![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue)
![Framework: Arduino](https://img.shields.io/badge/framework-Arduino-teal)
![Version: 0.1.0](https://img.shields.io/badge/version-0.1.0-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

Integrates your ESP32 project with SimpleOTA in a few lines: check for an update, stream and flash it with on-the-fly SHA-256 verification, and let the library handle all the protocol bookkeeping. The application owns Wi-Fi and decides when to check; this library does the rest.

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
- [Configuration](#configuration)
- [Logging](#logging)
- [Partition table](#partition-table)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Features

- **Two-method integration:** `check()` then `apply()` is all most projects need.
- **Streaming download with on-the-fly SHA-256:** firmware is verified before the partition is committed; no second pass, no large RAM buffer.
- **NVS build-number persistence:** the library reads and writes the SimpleOTA-assigned build number automatically.
- **Status event reporting:** reports the full update lifecycle back to the SimpleOTA backend.
- **No third-party dependencies:** uses only libraries bundled with Arduino-ESP32 core (`HTTPClient`, `Update`, `NetworkClientSecure` / `WiFiClientSecure`, `Preferences`, `mbedtls`).
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
| [`examples/AdvancedOTA/AdvancedOTA.ino`](examples/AdvancedOTA/AdvancedOTA.ino) | Custom `deviceId` / `boardId` / `hardwareRevision`, `setAutoReboot(false)`, manual `report("rebooted")`, verbose logging. |

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

> **Note on `validated`:** this event is reported after `Update.end()` succeeds (meaning "the image was flashed cleanly"), not after a successful boot. Arduino mode has no first-boot validation mechanism; see [Limitations](#limitations).

---

### `bool report(const char* event, const char* reason = nullptr)`

Manually posts a single status event to `/api/v1/ota/status/`. Useful for custom lifecycle control flows. Returns `true` on HTTP 2xx with `"accepted": true`.

Requires a deployment context populated by a successful `check()`. The context is retained through `apply()` (so a post-apply `report("rebooted")` works) and is cleared on the next `check()` invocation. Outside that window, `report()` returns `false` without sending anything.

Full event vocabulary defined by the API:

```
offered  download_started  downloaded  flashed  rebooted  validated  failed  rolled_back
```

Events emitted automatically by `apply()`: `download_started`, `downloaded`, `flashed`, `validated`, `failed`.

---

### `void setCACert(const char* pemRootCA)`

Overrides the CA certificate used for HTTPS verification. By default the library uses the bundled `kSimpleOtaRootCA` (ISRG Root X1), which covers both the SimpleOTA API and its firmware storage. Call this only if you need to pin a different certificate.

---

### `void setAutoReboot(bool enabled)`

Default: `true`. When `false`, `apply()` returns `OTA_SUCCESS` without calling `esp_restart()`, giving the application control over when the reboot happens. The NVS build number write and `validated` event still occur before the function returns.

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
| `SimpleOTAClient::SECURITY_MODE_SIGNED` | `"signed"` | Firmware signature verification. Coming soon. |

A raw string literal is also accepted for forward compatibility.

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

### `void begin(...)`

Starts a FreeRTOS background task (stack: 8 KB, priority: 1) that runs the following loop:

1. If `isConnected` was supplied, wait until it returns `true` (polled every second). Otherwise skip this step.
2. Call `check()`. If an update is available, call `apply()`.
3. If `onResult` is set and `apply()` returned without rebooting, invoke it with the `OTAResult`.
4. Sleep for `checkIntervalSec` seconds, then repeat.

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
check()  →  download_started  →  downloaded  →  flashed  →  validated  →  [esp_restart()]
```

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

---

## Configuration

Override any of the following with a `-D` compiler flag or a `#define` placed **before** `#include <SimpleOTAClient.h>`:

| Define | Default | Description |
| --- | --- | --- |
| `SIMPLEOTA_TIMEOUT_MS` | `15000` | Milliseconds before an HTTP request or a stalled download is abandoned. |
| `SIMPLEOTA_CHECK_INTERVAL_S` | `3600` | Default check interval (in seconds) used by `begin()`. Also exposed as a constant for manual scheduling in polling mode. |
| `SIMPLEOTA_DEBUG` | `0` | Compile-time default for verbose `[SimpleOTAClient]` logging on `Serial`. Prefer the runtime `SimpleOTAClient::setDebug(true)` toggle from your sketch; see [Logging](#logging). |

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
| No rollback / first-boot validation | `validated` means "flashed cleanly", not "booted successfully". There is no automatic rollback if the new image fails to boot. This is an inherent constraint of Arduino. |
| No automatic `rebooted` event | The library transitions directly from `validated` to `esp_restart()`. Applications that disable auto-reboot can call `report("rebooted")` explicitly before restarting; see AdvancedOTA. |
| `report()` requires a deployment context | The method needs a `deployment_id`, which only exists after a successful `check()`. The deployment context is retained through `apply()` so post-apply events (e.g. `"rebooted"`) work, but `report()` returns `false` before any `check()` has succeeded. |
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

## License

MIT License. See [LICENSE](LICENSE) for the full text.
