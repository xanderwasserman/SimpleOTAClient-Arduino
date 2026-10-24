# Changelog

## 1.0.0

The `SimpleOTAClient` class is replaced by the C API of the ESP-IDF component, `simpleota.h`. Both clients are now [SimpleOTAClient-core](https://github.com/wassie-dev/simpleOTAClient-core), copied into `src/sota_core`, built from the same sources with the same features, defaults and behaviour. Code that uses SimpleOTA moves between Arduino and ESP-IDF unchanged.

### Upgrading

- **The repository moved to [wassie-dev/simpleOTAClient-Arduino](https://github.com/wassie-dev/simpleOTAClient-Arduino).** GitHub redirects the old URL, but update `lib_deps` entries that name it.
- **Devices update from 0.6.0 and earlier over the air**, mid-trial too. The NVS keys and their meanings are unchanged, and a trial that fails returns to the old release, which reads the rollback as it always has. 1.0.0 adds one key, `sota_image`, which older releases ignore.
- **Sketches move to the C API.** `#include <simpleota.h>`, fill a `simpleota_config_t` and call `simpleota_init()` early in `setup()`, then `simpleota_start()` for managed mode, or `simpleota_check()` and `simpleota_apply()` for polling. The README's *Upgrading from 0.x* section maps every 0.x call, and all five examples are rewritten.
- **`SECURITY_MODE_TOKEN` is gone.** `cfg.security_mode` is an enum, `simpleota_security_t`: `SIMPLEOTA_SECURITY_BASIC`, `SIMPLEOTA_SECURITY_SIGNED`, or left at zero to report none. The platform never used a reported "token", and it kept such a device from matching basic and signed builds. The platform no longer stores a reported "token" and has cleared the values it kept, so devices that reported it match basic builds again.
- **`SIMPLEOTA_SECURITY_SIGNED` needs a signing key.** Without one `simpleota_init()` returns `ESP_ERR_INVALID_ARG`: 0.x applied the signed builds such a device was offered with only a checksum check.
- **`report()` and `setRollbackEnabled()` are gone.** The library posts every status event itself, and every update runs as a trial and rolls back if it is not confirmed.
- **Logging goes through ESP-IDF's log** with the tag `simpleota`. `setDebug()` and `SIMPLEOTA_DEBUG` are gone: call `esp_log_level_set("simpleota", ESP_LOG_DEBUG)`. Warnings show without it.
- **Build-time knobs have the ESP-IDF names:** `-DCONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS` replaces `SIMPLEOTA_HTTP_TIMEOUT_MS` and `SIMPLEOTA_TIMEOUT_MS`; `SIMPLEOTA_CONFIRM_TIMEOUT_S` is `cfg.confirm_timeout_s`; `SIMPLEOTA_TRIAL_WDT_MARGIN_S` is gone, the margin follows the HTTP timeout. `-DCONFIG_SIMPLEOTA_DOWNLOAD_CHUNK` sizes the download buffer. `SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` and `SIMPLEOTA_DISABLE_TRIAL_WATCHDOG` are unchanged.
- **TLS is always verified**, against the ISRG Root X1 and X2 certificates unless `cfg.cert_pem` is set. To test against a local server, give it a certificate from your own CA and set that CA as `cert_pem`.
- **Older builds are refused.** `simpleota_apply()` returns `SIMPLEOTA_DOWNGRADE`. To go back, release the old code under a new, higher build number.
- **A sketch flashed over USB reports build 0**, so the server offers it its current release. Firmware that is flashed over USB as a release can declare its build in `cfg.image_build`; see the README for when to. Before, it kept reporting the build SimpleOTA last installed.
- **Going back to 0.6.0 or earlier over USB:** those releases do not know `sota_image`, so they keep reporting the build 1.0.0 stored and are offered nothing until a higher build ships. Erase NVS when you flash one, or release the build you want as a new, higher one.
- `simpleota_check()` and `simpleota_apply()` refuse to run while the managed task runs, and `simpleota_set_server_url()` refuses until `simpleota_stop()`. Use polling or managed mode, not both.

### Added

- Lifecycle events through `cfg.event_cb`: download progress, failure reasons and the trial's outcome.
- `simpleota_check(&update)` returns `ESP_FAIL` when the server did not answer, where 0.x's `check()` returned false for that and for no update alike.
- `SIMPLEOTA_API_VERSION`, to test at compile time for something added in a later 1.x release. `simpleota.h` states what every 1.x release keeps: a config field left at zero gets its default, nothing is changed or removed, and the `reason` tokens it lists stay.
- `simpleota_check_now()` wakes the managed task for an immediate check, and `simpleota_current_build()` returns the build the device reports.
- Anti-rollback: an offer whose build number is not above the running build is refused, and the server gets `failed` with `downgrade_refused`, once per boot. An old image's signature still verifies, so this stops an old, vulnerable build being pushed.
- `cfg.image_build`: the build an image reports when it was flashed some other way than SimpleOTA, such as over USB. The running image's ELF SHA-256 is stored as `sota_image` to tell those images apart, so a device reused for another project no longer reports, and refuses offers below, the old project's build.
- ISRG Root X2 is trusted alongside X1, for servers with ECDSA certificates.
- Each check logs one line at info level: `check: no update (...)`, `check: offer build=N ...` or `check: failed (HTTP N)`.

### Changed

- `simpleota_check()` and `simpleota_apply()` run on a short-lived task with `CONFIG_SIMPLEOTA_TASK_STACK_SIZE` while the caller waits, so TLS and signature checks never depend on the caller's stack.
- HTTP goes through `esp_http_client` from the ESP-IDF inside Arduino-ESP32, the same transport as the ESP-IDF component, instead of `HTTPClient` and `WiFiClientSecure`.
- `chip_family` is optional and defaults to the chip the sketch is built for.
- `simpleota_current_build()` is right from `simpleota_init()` on; 0.x's `currentBuild()` returned 0 until the first check or confirm.
- With `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK`, the chip watchdog still runs, and the library rolls back a trial itself, as the bootloader would: on any reset before the confirm (a crash, a watchdog, a brownout, a power loss, `ESP.restart()` or deep sleep). A rollback cut short by a power loss is finished on the next boot.
- A trial that boots after a panic or watchdog reset is rolled back, also when the bootloader resumed it.
- `simpleota_stop()` lets the managed task finish its current step instead of deleting it mid-download, which could cut a flash write short or leak its TLS session.
- One client runs per device: a second `simpleota_init()` returns `ESP_ERR_INVALID_STATE`.
- In polling mode, the confirm timeout rolls back from a short-lived task.
- Firmware downloads follow up to three redirects, each only to another `https://` URL (`redirect_not_https` otherwise).
- The update slot is erased as the download is written, not all at once before it, and an image larger than the slot is refused before the download.
- Server responses are read from their top-level fields only, and numbers too large for 32 bits are refused.
- Labels that are not one JSON object are left out of the check-in with a warning, instead of making every check-in invalid.
- The watchdog margin grows with `CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS`, so a trial can finish its current request and roll back first. The trial's retry interval is fixed at 10 s.
- About 17 KB less flash for each example, and the same static RAM (core 2.0.17, ESP32).

## 0.6.0

### Added

- `setServerUrl()` lets `check()` and status reports target another https SimpleOTA server. Firmware downloads still use the pre-signed URL from the offer, and an invalid URL is rejected while the previous one is kept.

## 0.5.0

### Upgrading

- Updates from 0.4.0 are protected by the new rollback: the trial record keeps the NVS key layout of earlier releases, and a rollback to 0.4.0 behaves as it did before.
- Polling sketches must call `confirmRunning()` within the confirm time. A sketch that first calls the library later than the confirm time plus the watchdog margin is reset by the chip watchdog and rolled back.
- Sketches that deep-sleep must call `confirmRunning()` before the first sleep after an update; waking from deep sleep before that rolls back.
- The library defines `verifyRollbackLater()`. A sketch that defines its own must build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK`.
- `apply()` can return two new results: `OTA_REBOOT_PENDING` and `OTA_UNCONFIRMED`.

### Added

- A SimpleOTA trial stays on probation in the bootloader until `confirmRunning()`: the library defines `verifyRollbackLater()`, which holds only SimpleOTA trials. A crash or reset at any point before confirmation, including in a global constructor, returns to the previous firmware. Build with `-DSIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK` to keep the core's default.
- A hang during a trial is reset by the chip's RTC or LP watchdog, armed before `setup()` for the confirm time plus a margin, and the bootloader returns to the previous firmware; the report says `watchdog`. It stops on `confirmRunning()` and, on core 3.x, before deep sleep. A confirm time past the watchdog's limit (about 8 h) prints a warning. Build with `-DSIMPLEOTA_DISABLE_TRIAL_WATCHDOG` to leave it out.
- `rolled_back` reports carry the reset that ended the trial (`panic`, `watchdog`, `brownout`, and so on) or `confirm_timeout`.
- An unconfirmed trial image that wakes from deep sleep goes back to the previous firmware and reports `deep_sleep`. The core's bootloader resumes the image on a deep-sleep wake, so without this a device that sleeps before confirming would get a new confirm time on every wake and never roll back.
- `apply()` returns `OTA_UNCONFIRMED` during a trial, without downloading, so the previous firmware stays available to return to.
- With `setAutoReboot(false)`, `apply()` returns `OTA_REBOOT_PENDING` while a flashed update waits for `rebootForUpdate()`, and does not download. A second download would have overwritten the waiting update.

### Changed

- `confirmed` and `rolled_back` reports go to an outbox in NVS (two slots, oldest dropped first) and are sent at the start of each `check()`. A report counts as delivered on any HTTP response, and a pending report never holds up an update.
- `confirmRunning()` tells the bootloader first and returns `false`, with the trial still running, if it cannot.

### Fixed

- The trial record is saved before `apply()` switches the boot slot. A power cut during an update no longer leaves the new image running without a trial.
- Check intervals, the initial delay, and the confirm timeout are converted to FreeRTOS ticks in 64 bits. Durations longer than about 71 minutes no longer wrap to a much shorter wait.
- A deep-sleep wake that resumes the old image after `setAutoReboot(false)` keeps the flashed update waiting instead of reporting it as rolled back.
- A trial record from a boot with rollback enabled is read correctly after rollback is disabled: a device back on the previous image reports `rolled_back` instead of confirming an update that did not run.
- A second `SimpleOTAClient` in the boot that flashed a deferred update keeps the update waiting instead of reporting a false `rolled_back`.
- `rebootForUpdate()` reports the reboot under the waiting update's deployment, also after a later `check()` has fetched a newer offer.
- The RollbackOTA example runs its health-gated `confirmRunning()`. With managed auto-confirm off and that call commented out, every update it installed rolled back.

## 0.4.0 (2026-07-12)

- On-device Ed25519 firmware signature verification for signed artifacts, with up to two pinned keys for rotation.

## 0.3.0 (2026-05-19)

- `begin()` takes an `initialDelayMs` to defer the first check, so it does not start at the same time as other TLS sessions at boot.

## 0.2.0 (2026-05-12)

- Trial install with timeout-based rollback, `confirmed` and `reboot` lifecycle events, and `rebootForUpdate()`.
- RollbackOTA example.

## 0.1.0 (2026-05-10)

- First release.
