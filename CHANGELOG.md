# Changelog

## 0.5.0

A new SimpleOTA firmware stays unconfirmed until `confirmRunning()` runs, or until managed mode confirms it after the first successful check. A crash, a reset, or a hang during that time switches the board back to the previous firmware.

Polling sketches that used to call the library for the first time hours after boot will be reset by the chip watchdog unless they call `confirmRunning()` within the confirm time. BasicOTA and AdvancedOTA call `confirmRunning()` from `setup()`.

`apply()` returns `OTA_UNCONFIRMED` when the running firmware has not been confirmed. That return does not send a `failed` event. The offer is kept. Images written by ArduinoOTA or HTTPUpdate are accepted by the Arduino core on boot, because this library only holds a firmware open when it wrote its own trial note.

## 0.4.0

On-device Ed25519 signature verification for signed artifacts.

## 0.2.0

Library-managed trial install with a confirm timeout.
