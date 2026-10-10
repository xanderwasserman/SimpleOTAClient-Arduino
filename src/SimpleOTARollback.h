/**
 * SimpleOTARollback.h - host-testable trial decisions.
 *
 * No Arduino or ESP-IDF types. SimpleOTAClient.cpp applies the result:
 * it marks the running image valid, starts or stops the chip watchdog,
 * and keeps the saved trial record. The native test includes this header.
 *
 * Keep verifyRollbackLater() in SimpleOTAClient.cpp. A separate file would
 * be left out of a PlatformIO archive unless something else pulled it in,
 * and the strong symbol would not replace the core's weak one.
 */

#ifndef SIMPLEOTA_ROLLBACK_H
#define SIMPLEOTA_ROLLBACK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * True only for a SimpleOTA trial this boot should keep unconfirmed:
 * the running image is awaiting acceptance, the saved note says trial,
 * a previous partition was recorded, and we are not already running it.
 * verifyRollbackLater() returns this. Every other boot returns false so
 * the Arduino core accepts the image, including one written by ArduinoOTA
 * or HTTPUpdate.
 */
static inline bool sotaHoldPendingImage(
    bool imagePending,
    bool nvsOpen,
    uint8_t trial,
    uint32_t prevPart,
    bool runningMatchesPrev)
{
    return imagePending && nvsOpen && trial == 1
        && prevPart != 0 && !runningMatchesPrev;
}

/** True when a pending image should be marked valid on this boot. */
static inline bool sotaShouldMarkImageValid(
    bool imagePending,
    bool nvsOpen,
    uint8_t trial,
    bool rollbackEnabled,
    uint32_t prevPart,
    bool runningMatchesPrev)
{
    if (!imagePending) return false;
    if (sotaHoldPendingImage(imagePending, nvsOpen, trial, prevPart,
                             runningMatchesPrev)
        && rollbackEnabled) {
        return false;
    }
    return true;
}

/**
 * What processBootValidation() does with a saved trial record.
 *
 * SOTA_TRIAL_NONE            No record, or nothing this boot must change.
 * SOTA_TRIAL_HOLD            Arm the confirm timer. Roll back only if the
 *                            image is still unconfirmed when it fires.
 * SOTA_TRIAL_CONFIRMED       The image is already valid and this is not the
 *                            previous partition. The trial record is leftover
 *                            from a confirm that lost power before the record
 *                            was cleared. Queue the confirmed report and do
 *                            not arm the timer or the watchdog.
 * SOTA_TRIAL_ROLLED_BACK     Already back on the previous partition, or a
 *                            rollback was recorded earlier (trial == 2).
 * SOTA_TRIAL_CLEAR           The snapshot is incomplete. Drop the record.
 * SOTA_TRIAL_ACCEPT_RESIDUAL Rollback was turned off. Accept the image and
 *                            drop the leftover trial record.
 */
enum SotaTrialBoot {
    SOTA_TRIAL_NONE = 0,
    SOTA_TRIAL_HOLD,
    SOTA_TRIAL_CONFIRMED,
    SOTA_TRIAL_ROLLED_BACK,
    SOTA_TRIAL_CLEAR,
    SOTA_TRIAL_ACCEPT_RESIDUAL,
};

static inline SotaTrialBoot sotaTrialBootAction(
    bool bootloaderRollbackCompiled,
    bool imagePending,
    bool nvsOpen,
    uint8_t trial,
    bool rollbackEnabled,
    uint32_t prevPart,
    bool runningMatchesPrev,
    bool haveRunningPartition)
{
    if (!nvsOpen || trial == 0) return SOTA_TRIAL_NONE;
    if (trial == 2) return SOTA_TRIAL_ROLLED_BACK;
    if (!rollbackEnabled) return SOTA_TRIAL_ACCEPT_RESIDUAL;
    if (trial != 1) return SOTA_TRIAL_NONE;
    if (!haveRunningPartition || prevPart == 0) return SOTA_TRIAL_CLEAR;
    if (runningMatchesPrev) return SOTA_TRIAL_ROLLED_BACK;
    /* Bootloader rollback is off, so the image was never left unconfirmed.
       The saved record is the only trial, and the timer still has to run. */
    if (bootloaderRollbackCompiled && !imagePending) return SOTA_TRIAL_CONFIRMED;
    return SOTA_TRIAL_HOLD;
}

/**
 * One trial phase for the whole program. confirm, rollback, and
 * setConfirmTimeout() all read it. Only TRIAL can move, and only to
 * CONFIRMING or ROLLING_BACK, so the two writes cannot both proceed.
 */
enum SotaTrialPhase {
    SOTA_PHASE_IDLE = 0,
    SOTA_PHASE_TRIAL = 1,
    SOTA_PHASE_CONFIRMING = 2,
    SOTA_PHASE_ROLLING_BACK = 3,
};

enum SotaPhaseClaim {
    SOTA_CLAIM_CONFIRM = 0,
    SOTA_CLAIM_ROLLBACK = 1,
};

static inline bool sotaClaimPhase(uint8_t phase, int claim, uint8_t* next)
{
    if (!next || phase != SOTA_PHASE_TRIAL) return false;
    if (claim == SOTA_CLAIM_CONFIRM) {
        *next = SOTA_PHASE_CONFIRMING;
        return true;
    }
    if (claim == SOTA_CLAIM_ROLLBACK) {
        *next = SOTA_PHASE_ROLLING_BACK;
        return true;
    }
    return false;
}

/** setConfirmTimeout() may restart the timer and the watchdog only in TRIAL. */
static inline bool sotaTimeoutRearmAllowed(uint8_t phase)
{
    return phase == SOTA_PHASE_TRIAL;
}

/**
 * Confirm-timer period in FreeRTOS ticks. The multiply is 64-bit because
 * pdMS_TO_TICKS() multiplies in 32 bits and wraps past 4294 seconds when
 * the tick rate is 1000 Hz.
 */
static inline uint32_t sotaConfirmTimerTicks(uint32_t timeoutSec, uint32_t tickHz)
{
    uint64_t ticks = (uint64_t)timeoutSec * (uint64_t)tickHz;
    if (ticks > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return (uint32_t)ticks;
}

/** Extra watchdog time: the larger of floorSec and 10 percent of the deadline. */
static inline uint32_t sotaWatchMarginSec(uint32_t confirmSec, uint32_t floorSec)
{
    uint32_t tenth = confirmSec / 10u;
    return tenth > floorSec ? tenth : floorSec;
}

/**
 * Slow-clock ticks for a duration. calQ13_19 is the cached calibration
 * (microseconds per tick, Q13.19). Returns 0 when cal is 0 so the caller
 * can fall back to the nominal frequency.
 */
static inline uint64_t sotaSlowclkTicks(uint64_t timeoutUs, uint32_t calQ13_19)
{
    if (calQ13_19 == 0) return 0;
    return (timeoutUs << 19) / (uint64_t)calQ13_19;
}

/**
 * Nominal slow-clock ticks, used when the calibration value is still 0.
 * The result is clamped to the 32-bit stage and is at least 2 ticks.
 */
static inline uint32_t sotaNominalSlowTicks(uint64_t seconds, uint32_t hz)
{
    if (hz == 0) hz = 1;
    uint64_t ticks = seconds * (uint64_t)hz;
    if (ticks > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    if (ticks < 2) return 2;
    return (uint32_t)ticks;
}

/* Numeric values match esp_reset_reason_t in esp_system.h. */
enum {
    SOTA_RST_UNKNOWN    = 0,
    SOTA_RST_POWERON    = 1,
    SOTA_RST_EXT        = 2,
    SOTA_RST_SW         = 3,
    SOTA_RST_PANIC      = 4,
    SOTA_RST_INT_WDT    = 5,
    SOTA_RST_TASK_WDT   = 6,
    SOTA_RST_WDT        = 7,
    SOTA_RST_DEEPSLEEP  = 8,
    SOTA_RST_BROWNOUT   = 9,
};

/**
 * Reason string for a rolled_back report. A reason stored before this
 * library reboots wins, so a software reset from the confirm timer stays
 * confirm_timeout. Otherwise the reset that entered the bootloader is used.
 */
static inline const char* sotaRollbackReason(int resetCode, const char* stored)
{
    if (stored && stored[0] != '\0') return stored;
    switch (resetCode) {
        case SOTA_RST_PANIC:     return "panic";
        case SOTA_RST_INT_WDT:   return "int_wdt";
        case SOTA_RST_TASK_WDT:  return "task_wdt";
        case SOTA_RST_WDT:       return "watchdog";
        case SOTA_RST_BROWNOUT:  return "brownout";
        case SOTA_RST_POWERON:   return "power_on";
        case SOTA_RST_DEEPSLEEP: return "deep_sleep";
        case SOTA_RST_SW:        return "reset";
        default:                 return "reset";
    }
}

#endif  // SIMPLEOTA_ROLLBACK_H
