# SimpleOTAClient

> Thin Arduino-ESP32 client library for the [SimpleOTA](https://simpleota.com) firmware update platform.

![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue)
![Framework: Arduino](https://img.shields.io/badge/framework-Arduino-teal)
![Version: 1.0.0](https://img.shields.io/badge/version-1.0.0-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

Integrates your ESP32 project with SimpleOTA in a few lines: check for an update, stream and flash it with on-the-fly SHA-256 verification, and let the library handle all the protocol bookkeeping. The application owns Wi-Fi and decides when to check; this library does the rest.

Local OTA (ArduinoOTA, espota, ElegantOTA) is for a board on your desk. SimpleOTA is managed fleet OTA for ESP32 when those devices are in the field, without pulling you into a full IoT platform.

You upload a .bin from PlatformIO, ESP-IDF, Arduino, or CI, and devices check in on their own. Eligible ones get a short-lived pre-signed URL and pull firmware straight from object storage. Rollouts can start at 5% with deterministic bucketing, so the same unit stays in the same cohort. Builds can be Ed25519-signed and verified on the device during download, before the new partition is marked bootable. After reboot there's a trial window: crash or no successful check-in, and the device rolls back.

Signing here protects the delivery path. It is not Secure Boot. They stack if you already use Secure Boot.

[Quickstart (project, client, first update)](https://wiki.simpleota.com/getting-started/?utm_source=arduino_lib&utm_medium=readme&utm_campaign=signup_v1)

Starting a new project? [simpleOTA-starter-arduino](https://github.com/wassie-dev/simpleOTA-starter-arduino) is a template repo with a working sketch and a GitHub Actions release workflow already set up.

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
- [Build options](#build-options)
- [Update lifecycle](#update-lifecycle)
- [Security](#security)
  - [Signed firmware](#signed-firmware)
- [Rollback](#rollback)
  - [Testing rollback](#testing-rollback)
- [Anti-rollback](#anti-rollback)
- [Logging](#logging)
- [Partition table](#partition-table)
- [Footprint](#footprint)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [Arduino and ESP-IDF](#arduino-and-esp-idf)
- [Upgrading from 0.x](#upgrading-from-0x)
- [License](#license)

---

## Features

- **The same API as the ESP-IDF component:** `simpleota.h`, built from the same sources, so code that uses SimpleOTA moves between Arduino and ESP-IDF unchanged.
- **Managed or polling:** `simpleota_start()` runs the checks from the library's own task; or call `simpleota_check()` then `simpleota_apply()` yourself.
- **Streaming download with on-the-fly SHA-256:** firmware is verified before the partition is committed; no second pass, no large RAM buffer.
- **NVS build-number persistence:** the library reads and writes the SimpleOTA-assigned build number automatically.
- **Status event reporting:** reports the full update lifecycle back to the SimpleOTA backend.
- **On-device firmware signature verification:** signed artifacts are verified against your pinned Ed25519 public key, streamed during download, before the image is ever marked bootable. See [Signed firmware](#signed-firmware).
- **Trial install with rollback:** every new image runs as a trial, and goes back to the previous image if it crashes, hangs, or is not confirmed within the confirm timeout. See [Rollback](#rollback).
- **Anti-rollback:** an offer older than the running build is refused. See [Anti-rollback](#anti-rollback).
- **No external dependencies:** uses `esp_http_client`, OTA and NVS from the ESP-IDF inside Arduino-ESP32. The library is [SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core), copied into `src/sota_core`, with a vendored copy of [Monocypher](https://monocypher.org) 3.1.3 for Ed25519; nothing to install.
- **Compatible with Arduino-ESP32 2.x and 3.x**.
- **Secure by default:** TLS is always verified, against the bundled ISRG Root X1 and X2 certificates that SimpleOTA's hosts chain to; no configuration needed for production.

---

## Requirements

| Requirement | Version |
| --- | --- |
| Arduino-ESP32 core | 2.x or 3.x |
| Target hardware | Any ESP32 variant |
| Partition table | Must have two OTA app partitions (see [Partition table](#partition-table)) |
| Network | The sketch brings up IP connectivity; the library waits for it with `is_connected`, or checks once you call it |

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
    https://github.com/wassie-dev/simpleOTAClient-Arduino.git
```

---

## Quick start

### Managed mode

The library's own task checks on an interval, installs updates, and confirms each new image on its first successful check-in. The sketch's `loop()` is untouched.

```cpp
#include <WiFi.h>
#include <simpleota.h>

static bool wifiUp(void*) { return WiFi.status() == WL_CONNECTED; }

void setup() {
    Serial.begin(115200);
    WiFi.begin("your-ssid", "your-password");

    simpleota_config_t cfg = {};
    cfg.token = "soto_proj_xxxx";        // project or device token
    cfg.version_label = "1.0.0";         // shown on the dashboard
    cfg.is_connected = wifiUp;           // the task waits for an IP
    if (simpleota_init(&cfg) != ESP_OK || simpleota_start() != ESP_OK) {
        Serial.println("SimpleOTA did not start; see the log above");
    }
}

void loop() {
    // Your application; updates happen in the background.
}
```

Call `simpleota_init()` early in `setup()`: on the first boot of an update it starts the confirm timeout and the trial watchdog. Fill the config field by field from `{}`, as above, so it builds on every C++ version the cores use.

### Polling mode

The sketch runs the checks from its own loop, and confirms a new image itself once it works:

```cpp
void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    simpleota_confirm();                 // does nothing without a trial
    bool update = false;
    if (simpleota_check(&update) == ESP_OK && update) {
        simpleota_apply();               // restarts on success
    }
    delay(3600UL * 1000UL);
}
```

`examples/BasicOTA` is the complete sketch. In polling mode, an update that is never confirmed rolls back after the confirm timeout.

---

## Build numbers

SimpleOTA assigns its own **strictly monotonic build number** each time you upload a firmware artifact. The server uses this number (not your `APP_VERSION` string, not `__DATE__`) to decide whether a device needs an update.

**How the library manages it for you:**

| Situation | What happens |
| --- | --- |
| Factory-fresh device (empty NVS) | Library sends `current_build_number: 0` |
| After an update is installed | Library writes the build number from the check response into NVS (`simpleota` / `sota_build`, uint32) |
| With `disable_auto_reboot`, while the update waits for `simpleota_reboot_for_update()` | `sota_build` already holds the flashed build and checks send it, so the server does not offer that build again before the reboot |
| Every subsequent check | Library reads `sota_build` from NVS and sends it |
| Firmware installed another way (USB, ArduinoOTA, or the `Update` class) | The library sees the image changed without an update of its own and sends the build in [`image_build`](#when-to-set-image_build), or 0. With 0 the server offers the device its current release |

`simpleota_current_build()` returns this build once `simpleota_init()` has run, and 0 before.

**Do not write to the `simpleota`/`sota_build` NVS key from your own code.** Overwriting it with any value other than one the server gave you will break the server's comparison: the device will either never receive another update, or be offered a build it already has.

---

## API reference

The **[API reference](https://wassie-dev.github.io/simpleOTAClient-core/latest/)** documents every function, result, event and configuration field. It is generated from [`src/simpleota.h`](src/simpleota.h), which has the same comments. In short:

- Fill a [`simpleota_config_t`](https://wassie-dev.github.io/simpleOTAClient-core/latest/structsimpleota__config__t.html) from `{}` and call `simpleota_init()` early in `setup()`. Strings are copied. On Arduino, `chip_family` defaults to the chip the sketch is built for, and TLS is verified against the ISRG Root X1 and X2 certificates unless you set `cert_pem`.
- Managed mode: `simpleota_start()`, `simpleota_stop()`, `simpleota_check_now()`.
- Polling mode: `simpleota_check()`, then `simpleota_apply()`, from one task of yours, such as `loop()`.
- Trials: `simpleota_confirm()`, `simpleota_is_trial_boot()`.
- With `disable_auto_reboot`: `simpleota_reboot_for_update()`.
- Also `simpleota_set_server_url()`, `simpleota_current_build()`, `simpleota_last_offered_version()` and `simpleota_deinit()`.

`event_cb` runs on the task doing the work: keep it short, and call only `simpleota_reboot_for_update()` from it. The `reason` tokens that events and the dashboard show are listed under [Update lifecycle](#update-lifecycle) and [Rollback](#rollback).

---

## Build options

Build-time knobs, as `-D` flags (PlatformIO `build_flags`; in the Arduino IDE a `build_opt.h` next to the sketch). They have the same names and defaults as the ESP-IDF component's menuconfig options:

| Flag | Default | Description |
| --- | --- | --- |
| `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` | `15000` | Milliseconds before a request, or a download that receives nothing, is abandoned |
| `CONFIG_SIMPLEOTA_HTTP_TX_BUFFER` | `2048` | Must fit the request line of the long pre-signed download URLs |
| `CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK` | `4096` | The download buffer in bytes, on the heap during an update |
| `CONFIG_SIMPLEOTA_TASK_STACK_SIZE` | `12288` | The managed task's stack, also used by the short-lived tasks that run `simpleota_check()` and `simpleota_apply()`, a rollback, and the reboot |
| `CONFIG_SIMPLEOTA_TASK_PRIORITY` | `5` | The managed task's priority |
| `SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` | unset | Leave the chip watchdog out. Crashes and resets still roll back |
| `SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` | unset | Do not define `verifyRollbackLater()`; the library rolls back by itself, which misses an image that crashes before `simpleota_init()` on every boot |

The library's sources are C, so a flag must reach the C compiler: `build_flags` and `build_opt.h` both do.

**Retry policy:** check-ins and status posts retry once after a 2-second delay when no response arrives (not during a trial, which sends each once). They never retry a received HTTP response. The firmware download does not retry; a failure is reported and the next check can try again.

---

## Update lifecycle

Status events reported to the dashboard, in order:

```
download_started -> downloaded -> flashed -> validated -> reboot
    -> (next successful check-in) confirmed | rolled_back
```

Failures report `failed` with a reason token that the dashboard shows inline on the device row, and that `SIMPLEOTA_EVENT_UPDATE_FAILED` carries:

| Reason | Stage |
| --- | --- |
| `downgrade_refused` | The offered build is not above the running one (reported once per boot) |
| `insecure_url` | The download URL is not `https://` |
| `signature_invalid` | A signed offer failed Ed25519 verification, or a required signature is missing |
| `https_begin_failed` | The download connection could not be opened |
| `http_status_<N>` | The download returned HTTP N, or a fourth redirect |
| `redirect_not_https` | A download redirect led to a plain HTTP URL |
| `redirect_malformed` | A download redirect had no usable `Location` |
| `update_begin_failed` | The update slot could not be opened: no second OTA slot, or the image is larger than the slot |
| `update_write_failed` | A flash write failed during the download |
| `stream_error` | The connection broke during the download |
| `download_stalled` | No bytes arrived for `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` |
| `short_read` | The download ended before its size |
| `checksum_mismatch` | The SHA-256 of the download did not match the offer |
| `update_end_failed` | The image failed ESP-IDF's image check, or the boot slot could not be switched |
| `state_save_failed` | The trial record could not be saved to NVS before the slot switch |

Once a boot has worked out what happened to a trial, its `confirmed` or `rolled_back` report goes to an outbox in NVS (two slots, oldest dropped first) and never holds up an update. Reports are kept until the server answers, across reboots: any HTTP response counts as delivered, and the device does not act on what it says. Without a response the report is sent again, first after 10 s (three times), then at the normal check interval.

---

## Security

> **Read this section before deploying to a production fleet.**

The integrity guarantee of this library rests on the SHA-256 `checksum` field from the `/check/` response. That field travels over the same TLS channel as the firmware `url`. **If that TLS channel is not verified, an on-path attacker can supply any `(url, checksum)` pair (including for a malicious firmware image), and the device will flash it without complaint.**

### Default mode: secure

Without `cert_pem`, the library verifies TLS against the bundled ISRG Root X1 and X2 certificates. They cover both the SimpleOTA API and the firmware download endpoint. No additional configuration is needed for production.

### TLS is always verified

There is no way to turn verification off, and `base_url` must be `https://`.

To test against a local server, issue it a certificate from your own CA (for example with `mkcert`) for the hostname or IP the device connects to. For an IP, put it in the subjectAltName. Then set that CA as `cfg.cert_pem`.

### Other hardening built into the library

- Anti-rollback: an offer whose build number is not above the running build is refused (`downgrade_refused`), because an old image's signature still verifies and old builds can carry fixed bugs. To go back, release the old code under a new, higher build number. The running build is the one stored with the image SimpleOTA installed; an image flashed some other way, such as over serial, reports the build in `image_build`, or build 0, so a device reused for another project takes that project's builds.
- Download URLs that don't begin with `https://` are rejected before any connection is made.
- Project tokens containing CR/LF or control characters are rejected to prevent HTTP header injection.
- The library never retries a received HTTP response; only transport-level failures trigger a retry. This prevents fleet-scale retry storms.

### Signed firmware

The library verifies **Ed25519 firmware signatures on the device**, upgrading the integrity guarantee from "trust the TLS channel" to "trust only images approved by the holder of the project's private signing key". Even a fully compromised delivery path (server, storage, TLS) cannot make the device flash an unapproved image.

Setup ([full guide](https://wiki.simpleota.com/guides/signed-firmware/)):

```cpp
static const simpleota_signing_key_t kSigningKeys[] = {
    { "prod-2026",                       // the dashboard's key id, or nullptr
      "-----BEGIN PUBLIC KEY-----\n"
      "MCowBQYDK2VwAyEA...your project public key...\n"
      "-----END PUBLIC KEY-----\n" },
};

cfg.security_mode = SIMPLEOTA_SECURITY_SIGNED;
cfg.signing_keys = kSigningKeys;
cfg.num_signing_keys = 1;
```

`simpleota_init()` returns `ESP_ERR_INVALID_ARG` when a key does not parse. See `examples/SignedOTA`.

The signature (delivered in the `/check/` response for signed artifacts) is verified incrementally over the exact downloaded bytes, in the same streaming pass as the SHA-256 checksum, **before** the new image is marked bootable. No extra RAM buffer, no second pass over flash.

A device is **enforcing** once it both reports signed mode
(`SIMPLEOTA_SECURITY_SIGNED`) and has a key pinned. For an
enforcing device the decision is made from device state, not from what the
offer claims, so a compromised server or MITM cannot downgrade it by
stripping the signature fields:

| Offer | Enforcing? | Result |
| --- | --- | --- |
| Signed, valid signature | yes | Verified and flashed. |
| Signed, tampered signature | yes | Rejected before the image is bootable. `simpleota_apply()` returns `SIMPLEOTA_SIGNATURE_FAIL`; a `failed` event with reason `signature_invalid` is reported (shown on the dashboard device row). Device keeps its current firmware. |
| Signature missing, or offer arrives **without** signed fields | yes | Rejected before the download even starts (same `SIMPLEOTA_SIGNATURE_FAIL` / `signature_invalid`). This is the anti-downgrade guarantee. |
| Signed | no key pinned | Applied with checksum verification only, plus a logged warning on each such apply. Keeps fleet migration safe (below). |
| Not signed | not enforcing | Identical to previous releases. |

#### How the server matches modes

The server treats `security_mode` as a compatibility dimension: an artifact is only offered to a device when the two values are **exactly equal, or either side is blank**. A device that leaves `security_mode` unset reports nothing and matches every artifact. The rule is symmetric:

| Device reports | `basic` artifact | `signed` artifact |
| --- | --- | --- |
| *(nothing)* | offered | offered |
| `basic` | offered | **not offered** |
| `signed` | **not offered** | offered |

A blocked device does not error: it receives a normal `no_compatible_build` response and silently stays on its current build.

#### Migrating a fleet from `basic` to `signed`

Keys are compiled into the firmware, and the device's reported mode is changed by the firmware it runs, never by the mode of the artifact that delivered it. So:

1. Ship one final **`basic`** upload whose code adds the key to `signing_keys` and sets `security_mode` to `SIMPLEOTA_SECURITY_SIGNED`. A `basic` artifact reaches devices reporting `basic` *and* devices that never reported a mode, so the whole fleet gets it regardless of history.
2. Devices running it report `security_mode: "signed"` and verify every subsequent update on-device.
3. From then on, **upload only signed artifacts**: per the matrix above, a `signed` device is never offered a `basic` artifact, so there is no quiet fallback to unsigned releases.

Do not upload the migration build itself as `signed`: devices explicitly reporting `basic` will never be offered it, and devices that would accept it cannot verify it yet anyway.

> **Watch for stragglers.** After migrating, check the project device list for devices still reporting `basic` (stuck on an old build): they missed the migration artifact and will receive nothing once you publish only signed uploads.

**Key rotation:** create the new key server-side, ship a firmware update pinning **both** keys in `signing_keys`, start signing with the new key, then revoke the old key server-side and drop it from the next firmware.

#### Scope: this is not Secure Boot

Verification runs inside the application, so the trust anchor is the firmware the device is already running. That protects the **OTA delivery path** (compromised account, leaked upload token, tampered storage, MITM); it does **not** protect the device itself. An attacker with physical access can still reflash arbitrary firmware over UART/USB via the ESP32's ROM download mode, and firmware that is already compromised will not verify itself. The hardware answer to those threats is [ESP32 Secure Boot](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/secure-boot-v2.html) (plus Flash Encryption), which is complementary: SimpleOTA signing protects the supply chain to the device, Secure Boot protects the device itself. Note that stock Arduino-ESP32 cores ship precompiled bootloaders built **without** Secure Boot, so enabling it requires building under ESP-IDF; on the Arduino path, OTA-level signing is the strongest firmware-integrity protection available.

Ed25519 verification uses a vendored, unmodified copy of [Monocypher](https://monocypher.org) 3.1.3 (dual-licensed CC0-1.0 / BSD-2-Clause; see `src/sota_core/LICENSE-vendor.md`).

---

## Rollback

Every new image runs as a trial. Before the library switches the boot slot to it, it saves a trial record: the outgoing image's partition and identity (build number, hash, version label). A power cut at any point therefore leaves either the old image running or the new image on trial, never the new image unsupervised. The trial ends when the image is confirmed. If it is not confirmed within `confirm_timeout_s` (default 300 s), the library returns to the previous image and reboots into it.

The stock Arduino-ESP32 bootloader can switch back to the previous firmware by itself, but the core normally accepts every new image before `setup()` runs. The library tells the core to leave a SimpleOTA trial on probation (it defines `verifyRollbackLater()`), and only a SimpleOTA trial: an image written by ArduinoOTA or another updater is accepted as usual. So a crash or a reset at any point before the confirm, including in a global constructor or early in `setup()`, makes the bootloader return to the previous firmware on the next boot. The `rolled_back` report then carries the reset that ended the trial: `panic`, `int_wdt`, `task_wdt`, `watchdog`, `brownout`, `power_on`, `deep_sleep` or `reset`. Deep sleep also ends a trial: an image that wakes from deep sleep before it is confirmed goes back to the previous firmware, with the reason `deep_sleep`, so confirm before sleeping. `simpleota_confirm()` tells the bootloader the image is good first; if that fails, it returns `ESP_FAIL` and the trial carries on.

A sketch cannot define its own `verifyRollbackLater()` alongside the library. Build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` to leave the core's behaviour alone. The library then rolls back itself, as the bootloader would: on the confirm timeout, and on any reset before the confirm (a crash, a watchdog, a brownout, a power loss, `ESP.restart()` or deep sleep), so confirm before restarting on purpose. It misses an image that crashes before `simpleota_init()` on every boot. See [Limitations](#limitations).

A hang that also stops the FreeRTOS confirm timer is caught by the chip's own watchdog (the RTC or LP watchdog, which runs on the slow clock). It starts in `verifyRollbackLater()` before `setup()` for the default confirm time of 300 s, restarts with `confirm_timeout_s` in `simpleota_init()`, and stops on the confirm. It waits for the confirm time plus a margin: the larger of 10 percent and the time one request can take at `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` (70 s at the default), and never less than 60 s. When it fires, the chip resets, the previous firmware comes back, and the report says `watchdog`. So call `simpleota_init()` early and confirm within the confirm time. At the usual internal slow clock the watchdog can wait at most about 8 hours (about 13 hours on the S2); a longer confirm time logs a warning and the watchdog fires at its limit. On core 3.x it stops before deep sleep, so the wake reports `deep_sleep`; on core 2.x a sleep longer than the rest of the confirm time ends in a watchdog reset instead. Either way the trial rolls back, so confirm before sleeping. Build with `-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG` to leave it out.

While a trial is running, no further update is downloaded: the other slot holds the previous image the trial may still return to.

### Polling mode

You must call `simpleota_confirm()` yourself. The natural place is at the top of `loop()`, *after* the sketch has proven it can do real work:

```cpp
void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    simpleota_confirm();   // nothing happens without a trial
    // ... your app code ...
    bool update = false;
    if (simpleota_check(&update) == ESP_OK && update) simpleota_apply();
    delay(3600UL * 1000UL);
}
```

If you never call it, every update rolls back.

### Managed mode

The managed task confirms the trial on its first successful check-in, which proves boot, connectivity, TLS, token auth and server reachability. No sketch code is needed.

To confirm after your own health check instead:

```cpp
cfg.manual_confirm = true;
cfg.confirm_timeout_s = 120;   // give the sketch 120 s to confirm
// ...
if (simpleota_is_trial_boot() && applicationHealthy()) {
    simpleota_confirm();
}
```

See `examples/RollbackOTA` for the full pattern.

### Cellular and other high-latency transports

The default 300 s timeout assumes Wi-Fi. For cellular or other transports where attaching, registering, and reaching the API can routinely take several minutes, raise `confirm_timeout_s` accordingly.

### Server interaction

After a rollback the previous image's identity (build number, hash, version label) is restored in NVS, and a `rolled_back` report with the failed deployment's `deployment_id` and `build_number` is queued. Its reason is `confirm_timeout` when the confirm timer ran out, otherwise the reset that ended the trial (see above). On a successful trial, a `confirmed` report is queued. Both go through the outbox described in [Update lifecycle](#update-lifecycle).

### Testing rollback

Before relying on rollback in the field, try each way a trial can fail on one device of every board you ship. Deploy a known-good build first, then try each of these from it and watch the serial log and the dashboard:

| Test | Expected result |
| --- | --- |
| Confirms as usual | Stays on the new build; `confirmed` |
| Never confirms (managed mode with `manual_confirm`) | Back on the known-good build after the confirm time; `rolled_back` with `confirm_timeout` |
| Calls `abort()` early in `setup()` | Back on the known-good build after one reset; `rolled_back` with `panic` |
| Hangs in `setup()` (`for (;;) {}`) | Back on the known-good build; `rolled_back` with `confirm_timeout`, or with the watchdog reset that caught the hang |
| Calls `disableCore0WDT()`, then starts a task at the highest priority on each core that spins forever, so the confirm timer cannot run either | Back on the known-good build after the confirm time plus the watchdog margin; `rolled_back` with `watchdog` |
| Loses power before the confirm | Back on the known-good build; `rolled_back` with `power_on` |
| Loses power during the download | Stays on the known-good build and downloads the update again on a later check |

Keep the confirm time short while testing, for example `cfg.confirm_timeout_s = 60`, and turn on [logging](#logging) to see each boot classified. Run the table once on a device that still has 0.6.0, so the first update into this release, and a rollback to 0.6.0, are covered too. If your devices use deep sleep, also deep-sleep a test build before it confirms: it should wake on the known-good build, with `rolled_back` and `deep_sleep`.

---

## Anti-rollback

An offer whose build number is not above the running build is refused (`downgrade_refused`, `SIMPLEOTA_DOWNGRADE`), because an old image's signature still verifies and old builds can carry fixed bugs. To go back, release the old code under a new, higher build number.

The running build is the one stored with the image SimpleOTA installed. An image flashed some other way, such as over USB, reports build 0, so the server offers it the current release and a device reused for another project takes that project's builds.

### When to set `image_build`

Most projects never need it. Leave it at 0 while you develop and test, and whenever devices only get firmware from SimpleOTA.

It is for one situation: you flash a **release** image over USB, such as at the factory or when re-flashing a unit in the field, and the same file is also a SimpleOTA upload. Without it the device reports build 0, so on its first check-in the server sends it the current release again, or an older one if that is what its deployment targets. With it, the device reports the build it actually runs and is left alone.

To use it:

1. Look up the project's last build number on the dashboard, say 41. SimpleOTA numbers each upload one above the last, so the next upload will be 42.
2. Set `cfg.image_build = 42` and build.
3. Upload that exact `.bin` to SimpleOTA, and check that it got build 42.
4. Flash the same `.bin` over USB.

If the number is wrong, the device reports a build it does not run. Set too high, it is not offered any build up to that number, so set it back to 0 in the next upload whenever you are not sure.

---

## Logging

The library logs through ESP-IDF's log, with the tag `simpleota`, on the same serial port as `Serial`. Warnings and errors, such as a rejected token, always show. For every step, after `simpleota_init()`:

```cpp
esp_log_level_set("simpleota", ESP_LOG_DEBUG);   // or ESP_LOG_INFO for one line per check
```

`#include <esp_log.h>` for it. Each check logs `check: no update (up_to_date)`, `check: offer build=N ...` or `check: failed (HTTP N)` at info level.

---

## Partition table

A scheme works for SimpleOTA when its CSV defines both `ota_0` and `ota_1` (see `tools/partitions/` in [Arduino-ESP32 3.3.12](https://github.com/espressif/arduino-esp32/tree/3.3.12/tools/partitions)). Choose the Partition Scheme entry in the Tools menu, or set `board_build.partitions`. On 4 MB boards, common choices include *Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)* (`default.csv`, 1280 KB per slot), *Default 4MB with ffat (1.2MB APP/1.5MB FATFS)* (`default_ffat.csv`, 1280 KB), and *Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)* (`min_spiffs.csv`, 1920 KB; some boards label this *Minimal SPIFFS (Large APPS with OTA)*). *Huge APP*, *Minimal (1.3MB APP/700KB SPIFFS)* and the plain *No OTA* entries have one app slot, so `simpleota_apply()` reports `update_begin_failed`.

### Changing the scheme

A normal Arduino IDE or arduino-cli USB sketch upload uses esptool to write the bootloader, `partitions.bin` at `0x8000`, `boot_app0.bin` at `0xe000`, and the app, so the selected partition scheme is applied. Network OTA, including a SimpleOTA update, writes only the app image, so the scheme cannot be changed over the air. If you are moving off a single-slot scheme, enable *Erase All Flash Before Sketch Upload* for that USB flash (PlatformIO: `pio run -t erase` first) so NVS and stored build numbers reset cleanly. Boards already on a two-slot layout do not need a full erase just to upload a new sketch.

For mixed schemes in one fleet, set [`partition_profile`](https://wassie-dev.github.io/simpleOTAClient-core/latest/structsimpleota__config__t.html) to matching artifact profiles. A blank `partition_profile` on the artifact or on the device matches any value for that field only; `chip_family`, `board_id`, `framework` and `hardware_revision` still filter offers.

---

## Footprint

Built with Arduino-ESP32 2.0.17 for the ESP32, the examples take about 17 KB less flash than with 0.6.0, and the same static RAM. The library uses `esp_http_client`, not `HTTPClient`, so a sketch that also uses `HTTPClient` links both HTTP stacks, about 23 KB more than it would with one. mbedTLS is shared.

---

## Limitations

| Limitation | Detail |
| --- | --- |
| Hangs before `verifyRollbackLater()` | A hang in a global constructor happens before the chip watchdog starts, so nothing resets it. A crash there still rolls back. |
| One client per device | The trial record and the boot slot belong to the device. A second `simpleota_init()` returns `ESP_ERR_INVALID_STATE`. |
| No automatic `reboot` event with `disable_auto_reboot` | The library only reports `reboot` on the reboot it controls. Call `simpleota_reboot_for_update()`, which reports it and then restarts, instead of `ESP.restart()`; see AdvancedOTA. |
| The sketch owns connectivity | The library works over any network interface and does not manage Wi-Fi, Ethernet, PPP, or reconnects. Give it `is_connected`, or check only once there is an IP. |
| Default CA | The ISRG Root X1 and X2 certificates cover current SimpleOTA infrastructure. If the platform moves to a host under another root, set `cert_pem`, or update the library. |

---

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `simpleota_check()` returns `ESP_FAIL` | No response, or an error status: no network, a bad token, or the server is down. Raise the [log level](#logging) and look for the `check:` line. |
| `simpleota_check()` returns `ESP_OK` but never offers an update | No active deployment targets this device, or the build it reports is already the latest. The `check:` line shows the server's status. |
| `update_begin_failed` | Partition table has no second OTA slot, or the OTA slot is too small for the firmware being applied. |
| `SIMPLEOTA_CHECKSUM_FAIL` | The firmware object was replaced or corrupted on the server between the check and the download. |
| Device is offered the same build repeatedly | The `simpleota`/`sota_build` NVS key was cleared or overwritten. See [Build numbers](#build-numbers). |
| A sketch flashed over USB is replaced by the server's release | Expected: the device reports build 0 for an image SimpleOTA did not install. Only if you flash release images over USB, see [When to set `image_build`](#when-to-set-image_build). |
| `simpleota_apply()` returns `SIMPLEOTA_DOWNGRADE` | The server offered a build that is not above the one the device runs. To go back, upload the old code again as a new build. |
| Download stalls or times out | Weak Wi-Fi signal, or `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` is too short for the link. Increase the timeout or improve signal quality. |
| `simpleota_init()` returns `ESP_ERR_INVALID_ARG` | A missing token or one with control characters, a `base_url` that is not `https://`, a signing key that does not parse, or `SIMPLEOTA_SECURITY_SIGNED` without a signing key. The log says which. |

---

## Arduino and ESP-IDF

The [ESP-IDF component](https://github.com/wassie-dev/simpleOTAClient-ESP-IDF) is this same client: the same `simpleota.h`, built from the same sources, so code that uses SimpleOTA works unchanged in an ESP-IDF app. Two things differ, because Arduino-ESP32 has no Kconfig and no certificate bundle: the build-time knobs are `-D` flags here and menuconfig options there, with the same names and defaults; and without `cert_pem`, this library verifies TLS against the ISRG roots while the component uses ESP-IDF's certificate bundle.

---

## Upgrading from 0.x

Devices update from 0.6.0 and earlier over the air, also in the middle of a trial: the NVS layout is unchanged, and a trial that fails returns to the old release. 1.0.0 adds one NVS key, `sota_image`, which older releases ignore.

The sketch changes: 1.0.0 replaces the `SimpleOTAClient` class with the C API that the ESP-IDF component has. `#include <simpleota.h>` instead of `<SimpleOTAClient.h>`, fill a `simpleota_config_t`, and call `simpleota_init()` early in `setup()`.

| 0.x | 1.0.0 |
| --- | --- |
| `SimpleOTAClient ota(token, chipFamily, deviceId, boardId, hardwareRevision)` | `cfg.token`, `cfg.chip_family`, `cfg.device_id`, `cfg.board_id`, `cfg.hardware_revision`, then `simpleota_init(&cfg)` |
| `ota.begin(interval, onResult)` / `begin(interval, isConnected, initialDelayMs)` | `cfg.check_interval_s`, `cfg.is_connected`, `cfg.initial_delay_ms`, then `simpleota_start()`; outcomes through `cfg.event_cb` |
| `ota.end()`, `ota.checkNow()` | `simpleota_stop()`, `simpleota_check_now()` |
| `ota.check()` returning `bool` | `simpleota_check(&update)`, returning `ESP_OK` when the server answered, with `update` set when it offers one |
| `ota.apply()` returning `OTAResult` | `simpleota_apply()` returning `simpleota_result_t` (`OTA_SUCCESS` is `SIMPLEOTA_OK`, `OTA_*` is `SIMPLEOTA_*`) |
| `ota.onEvent(cb)` | `cfg.event_cb`, with `simpleota_event_t` (`SIMPLEOTA_EVENT_*`) |
| `ota.confirm()` / `confirmRunning()` | `simpleota_confirm()`, `ESP_OK` or `ESP_FAIL` |
| `ota.isTrialBoot()` / `isTrialInstall()` | `simpleota_is_trial_boot()` |
| `ota.setManagedAutoConfirm(false)` | `cfg.manual_confirm = true` |
| `ota.setConfirmTimeout(s)`, `SIMPLEOTA_CONFIRM_TIMEOUT_S` | `cfg.confirm_timeout_s` |
| `ota.setAutoReboot(false)`, `ota.rebootForUpdate()` | `cfg.disable_auto_reboot = true`, `simpleota_reboot_for_update()` |
| `ota.setServerUrl(url)` | `cfg.base_url`, or `simpleota_set_server_url(url)` |
| `ota.setCACert(pem)` | `cfg.cert_pem` |
| `ota.setSigningPublicKey(pem)`, `addSigningPublicKey(id, pem)` | `cfg.signing_keys`, `cfg.num_signing_keys` |
| `setPartitionProfile`, `setNvsSchemaVersion`, `setLabels`, `setSecurityMode`, `setChannel`, `setVersionLabel`, `setImageBuild` | `cfg.partition_profile`, `cfg.nvs_schema_version`, `cfg.labels_json`, `cfg.security_mode`, `cfg.channel`, `cfg.version_label`, `cfg.image_build` |
| `SECURITY_MODE_BASIC`, `SECURITY_MODE_SIGNED`, `CHIP_*` constants | `SIMPLEOTA_SECURITY_BASIC`, `SIMPLEOTA_SECURITY_SIGNED`; `chip_family` defaults to the chip the sketch is built for |
| `SECURITY_MODE_TOKEN` | Gone: the platform never used it, and a device reporting it matched neither basic nor signed builds |
| `currentBuild()`, `lastOfferedVersion()` | `simpleota_current_build()`, `simpleota_last_offered_version()` |
| `SimpleOTAClient::setDebug(true)`, `SIMPLEOTA_DEBUG` | `esp_log_level_set("simpleota", ESP_LOG_DEBUG)` |
| `SIMPLEOTA_HTTP_TIMEOUT_MS` / `SIMPLEOTA_TIMEOUT_MS` | `-DCONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` |
| `report()`, `setRollbackEnabled()` | Gone: the library posts every status event itself, and every update runs as a trial |

What behaves differently:

- TLS is always verified. For a local server, set `cfg.cert_pem` to your own CA.
- `SIMPLEOTA_SECURITY_SIGNED` needs a signing key: without one, `simpleota_init()` fails, because the device would be offered signed builds it cannot verify.
- An offer whose build is not above the running one is refused (`SIMPLEOTA_DOWNGRADE`). See [Anti-rollback](#anti-rollback).
- A sketch flashed over USB reports build 0, so the server offers it the current release. If you flash release images over USB, see [When to set `image_build`](#when-to-set-image_build).
- `simpleota_check()` and `simpleota_apply()` don't run while the managed task runs.
- Going back to 0.6.0 or earlier over USB works, but those releases do not know `sota_image`, so they keep reporting the build 1.0.0 stored, the last one SimpleOTA installed, and are offered nothing until a higher build ships. Erase NVS when you flash one, or release the build you want as a new, higher one.

The [CHANGELOG](CHANGELOG.md) lists every change.

---

## License

MIT License. See [LICENSE](LICENSE) for the full text.
