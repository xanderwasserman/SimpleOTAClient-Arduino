/**
 * SimpleOTARollback.h - host-testable trial / pending-image policy.
 *
 * No Arduino or ESP-IDF types. SimpleOTAClient.cpp applies the result with
 * esp_ota_mark_app_valid_cancel_rollback(), the RTC watchdog, and the
 * existing NVS trial record. The native test includes this header directly.
 */

#ifndef SIMPLEOTA_ROLLBACK_H
#define SIMPLEOTA_ROLLBACK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * What to do with the running image's bootloader pending-verify state.
 *
 * SOTA_IMAGE_LEAVE  The running image is not pending. Do not call mark-valid
 *                   (the IDF call is only valid from PENDING_VERIFY).
 * SOTA_IMAGE_ACCEPT The image is pending and this is not a SimpleOTA trial
 *                   we must keep open (no trial record, rollback disabled,
 *                   incomplete snapshot, or we already booted back onto the
 *                   previous partition). Mark it valid.
 * SOTA_IMAGE_HOLD   Pending SimpleOTA trial on the new partition. Leave the
 *                   image pending so a reset rolls back in the bootloader.
 */
enum SotaImageDisposition {
    SOTA_IMAGE_LEAVE = 0,
    SOTA_IMAGE_ACCEPT,
    SOTA_IMAGE_HOLD,
};

/**
 * @param imagePending        Running image is ESP_OTA_IMG_PENDING_VERIFY.
 * @param nvsOpen             The simpleota namespace could be read.
 * @param trial               sota_trial: 0 none, 1 in trial, 2 rolled back.
 * @param rollbackEnabled     Runtime setRollbackEnabled() value.
 * @param prevPart            Saved previous partition address, or 0.
 * @param runningMatchesPrev  Running partition address equals prevPart.
 */
static inline SotaImageDisposition sotaPendingImageAction(
    bool imagePending,
    bool nvsOpen,
    uint8_t trial,
    bool rollbackEnabled,
    uint32_t prevPart,
    bool runningMatchesPrev)
{
    if (!imagePending) return SOTA_IMAGE_LEAVE;
    if (!nvsOpen) return SOTA_IMAGE_ACCEPT;
    if (!rollbackEnabled) return SOTA_IMAGE_ACCEPT;
    if (trial != 1) return SOTA_IMAGE_ACCEPT;
    if (prevPart == 0) return SOTA_IMAGE_ACCEPT;
    if (runningMatchesPrev) return SOTA_IMAGE_ACCEPT;
    return SOTA_IMAGE_HOLD;
}

/**
 * Hardware backstop decision for verifyRollbackLater(), which runs before
 * the sketch can change setRollbackEnabled(). Arm only when the running
 * image is pending and a SimpleOTA trial record was actually read.
 * Every other boot disarms, so a leftover RTC watchdog cannot fire on a
 * normal boot.
 */
enum SotaWatchdogAction {
    SOTA_WDT_DISARM = 0,
    SOTA_WDT_ARM,
};

static inline SotaWatchdogAction sotaTrialWatchdogAction(
    bool imagePending, bool nvsOpen, uint8_t trial)
{
    if (imagePending && nvsOpen && trial == 1) return SOTA_WDT_ARM;
    return SOTA_WDT_DISARM;
}

/** apply() refuses to flash a new image while the running one is still pending. */
static inline bool sotaApplyBlockedWhilePending(bool imagePending)
{
    return imagePending;
}

#endif  // SIMPLEOTA_ROLLBACK_H
