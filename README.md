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
- [Rollback](#rollback)
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
- **Trial install with timeout-based rollback:** the library snapshots the previous image before applying, then rolls back to it if `confirmRunning()` is not called within the configurable timeout. See [Rollback](#rollback).
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

### `void setRollbackEnabled(bool enabled)`

Enables or disables the library-managed trial-install / rollback machinery. Default: `true`. See [Rollback](#rollback). When `false`, `apply()` skips the pre-OTA snapshot and `confirmRunning()` always returns `false` (no trial is ever armed). The `confirmed` event is still emitted on the first 2xx `/check/` after a reboot, via the NVS-backed deferred path.

---

### `void setManagedAutoConfirm(bool enabled)`

In managed mode (`begin()`), controls whether the library automatically calls `confirmRunning()` on the first 2xx response from `/api/v1/ota/check/` after a trial boot. Default: `true`. Set to `false` to require an explicit `confirmRunning()` call from your application code instead. No effect in polling mode (where you always call `confirmRunning()` yourself).

---

### `void setConfirmTimeout(uint32_t seconds)`

Sets the per-instance trial-install confirm timeout in seconds. Defaults to `SIMPLEOTA_CONFIRM_TIMEOUT_S` (compile-time default `300`). Clamped to `[10, 86400]`. Effective starting with the next `apply()`. See [Rollback](#rollback).

---

### `bool confirmRunning()`

Confirms that the currently-running firmware is healthy and seals the trial install. Cancels the rollback timer, clears the trial snapshot (`prev_*` keys and trial flag) from NVS, and queues a `confirmed` status event for the next successful `/check/` round-trip. Returns `true` if a trial was in progress and is now confirmed; `false` if no trial was in progress (a normal boot — this is a safe no-op).

Call from your application code once you are satisfied the new firmware is working. In polling mode this is required after every successful OTA; in managed mode it is only required if you have called `setManagedAutoConfirm(false)`.

---

### `bool isTrialInstall()`

Returns `true` while a trial install is in progress (i.e. the current boot is the first boot of a new firmware image and `confirmRunning()` has not yet been called). Use this to avoid expensive health-probe code on normal boots.

---

### `void begin(...)`

Starts a FreeRTOS background task (stack: 8 KB, priority: 1) that runs the following loop:

1. If `isConnected` was supplied, wait until it returns `true` (polled every second). Otherwise skip this step.
2. **If a trial install is in progress**, run a short-retry inner loop: retry `/check/` every `SIMPLEOTA_TRIAL_RETRY_INTERVAL_S` seconds (default 10 s) until the server responds with 2xx or the confirm timeout expires. With `setManagedAutoConfirm(true)` (default), call `confirmRunning()` on the first 2xx and continue; the `confirmed` event fires on that same `/check/` response. Skip this step on a normal (non-trial) boot.
3. Call `check()`. If an update is available, call `apply()`.
4. If `onResult` is set and `apply()` returned without rebooting, invoke it with the `OTAResult`.
5. Sleep for `checkIntervalSec` seconds, then repeat.

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

If [rollback](#rollback) is enabled and a trial install is not confirmed within the timeout, the library reboots into the previous partition and — once the next `check()` succeeds — sends a separate `rolled_back` event with reason `confirm_timeout`.

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

## Rollback

Since v0.2.0 the library supports a **library-managed trial install with timeout-based rollback**. When `apply()` succeeds, the library snapshots the outgoing image's partition address and NVS metadata, then reboots into the new firmware. The new boot enters a "trial" state: if the application calls `confirmRunning()` before `SIMPLEOTA_CONFIRM_TIMEOUT_S` (default 300 s), the trial is sealed and the snapshot is discarded. Otherwise the library restores the previous partition and reboots back into it.

Stock Arduino-ESP32 builds do not have the IDF bootloader's pending-verify feature enabled, so this is implemented entirely in firmware rather than via `esp_ota_mark_app_valid_cancel_rollback()`. A consequence: a hard hang **before** the library's confirm-machinery runs (e.g. inside a global object constructor, or in `setup()` before `confirmRunning()` is reached) is still caught by the timeout, but a hang in the C runtime or bootloader is not. See [Limitations](#limitations).

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

The default 300 s timeout assumes Wi-Fi. For cellular or other transports where attaching, registering, and reaching the API can routinely take several minutes, raise `setConfirmTimeout()` accordingly. The clamp is `[10, 86400]` seconds.

### Server interaction

When the library rolls back, the previous boot's snapshot is restored to the partition and to NVS (build number, hash, version label). On the *next* boot, after `check()` makes its first successful round-trip, the library sends a `rolled_back` status event with the failed deployment's `deployment_id` and `build_number` and a reason of `confirm_timeout`. The event is retried on every subsequent `check()` until the server accepts it.

On a successful trial (device boots new image, `confirmRunning()` executes), the library queues a `confirmed` status event. It is sent on the first 2xx from `/check/` after `confirmRunning()` has run and is retried on every subsequent `check()` until the server accepts it. Both `confirmed` and `rolled_back` are persisted in NVS so they survive a power-cycle between the confirm/rollback event and the next network round-trip.

### Disabling rollback

If you do not want the rollback machinery at all:

```cpp
ota.setRollbackEnabled(false);
```

This skips the snapshot in `apply()` and the trial-boot arming entirely. Any residual trial state left in NVS from a previous boot with rollback enabled is cleaned up automatically by `processBootValidation()` on the next boot — no manual NVS clearing is needed.

---

## Configuration

Override any of the following with a `-D` compiler flag or a `#define` placed **before** `#include <SimpleOTAClient.h>`:

| Define | Default | Description |
| --- | --- | --- |
| `SIMPLEOTA_TIMEOUT_MS` | `15000` | Milliseconds before an HTTP request or a stalled download is abandoned. |
| `SIMPLEOTA_CHECK_INTERVAL_S` | `3600` | Default check interval (in seconds) used by `begin()`. Also exposed as a constant for manual scheduling in polling mode. |
| `SIMPLEOTA_CONFIRM_TIMEOUT_S` | `300` | Default trial-install confirm timeout (in seconds). See [Rollback](#rollback). Overridable per-instance via `setConfirmTimeout()`. |
| `SIMPLEOTA_TRIAL_RETRY_INTERVAL_S` | `10` | How often (in seconds) the managed task retries `/check/` while in a trial install. |
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
| Rollback gap before `confirmRunning()` is callable | The library-managed rollback (see [Rollback](#rollback)) requires the device to reach the point in `setup()` or `loop()` where `confirmRunning()` (polling) or the OTA task's first `/check/` (managed) runs. A crash before that point — e.g., in a constructor of a global object, or in `setup()` before connectivity comes up — is still caught by the timeout, but a hard hang in the bootloader or pre-`setup()` C runtime is not. Native ESP-IDF rollback (pending-verify) is not used because Arduino-ESP32 builds do not ship with bootloader rollback enabled. |
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

## License

MIT License. See [LICENSE](LICENSE) for the full text.
