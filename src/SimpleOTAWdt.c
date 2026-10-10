/*
 * Trial watchdog, in C. hal/wdt_hal.h does not compile as C++ on the S2
 * and the C3 in core 2.0.17.
 */

#include "sdkconfig.h"
#include "SimpleOTARollback.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef SIMPLEOTA_TRIAL_WDT_MARGIN_S
#define SIMPLEOTA_TRIAL_WDT_MARGIN_S 60
#endif

#if (defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)) \
    && !defined(SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK) \
    && !defined(SIMPLEOTA_DISABLE_TRIAL_WATCHDOG) \
    && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_C 1
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_HAVE_SLOWCLK_CAL 1
#else
#define SOTA_HAVE_SLOWCLK_CAL 0
#endif
#endif

#ifndef SOTA_WDT_C
#define SOTA_WDT_C 0
#endif

#if (defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)) \
    && !defined(SIMPLEOTA_DISABLE_BOOTLOADER_ROLLBACK) \
    && !defined(SIMPLEOTA_DISABLE_TRIAL_WATCHDOG) \
    && __has_include("hal/wdt_hal.h") \
    && !SOTA_WDT_C
#error "SimpleOTA trial watchdog was left out of this build"
#endif

static bool s_armed = false;

static void sota_wdt_hw_off(void) {
#if SOTA_WDT_C
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_disable(&hal);
    wdt_hal_write_protect_enable(&hal);
#endif
    s_armed = false;
}

#if SOTA_WDT_C
static uint32_t sota_wdt_ticks(uint32_t confirm_sec) {
    uint32_t margin = sotaWatchMarginSec(confirm_sec, SIMPLEOTA_TRIAL_WDT_MARGIN_S);
    uint64_t total_sec = (uint64_t)confirm_sec + (uint64_t)margin;
    uint64_t us = total_sec * 1000000ull;
#if SOTA_HAVE_SLOWCLK_CAL
    uint32_t cal = esp_clk_slowclk_cal_get();
    uint64_t calibrated = sotaSlowclkTicks(us, cal);
    if (calibrated != 0) {
        if (calibrated > 0xFFFFFFFFu) return 0xFFFFFFFFu;
        if (calibrated < 2) return 2;
        return (uint32_t)calibrated;
    }
#else
    (void)us;
#endif
    return sotaNominalSlowTicks(total_sec, rtc_clk_slow_freq_get_hz());
}
#endif

void sota_wdt_arm(uint32_t confirm_sec) {
#if SOTA_WDT_C
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, sota_wdt_ticks(confirm_sec),
                         WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
#else
    (void)confirm_sec;
    s_armed = false;
#endif
}

void sota_wdt_disarm(void) {
    if (!s_armed) return;
    sota_wdt_hw_off();
}

void sota_wdt_quiesce(void) {
    sota_wdt_hw_off();
}

bool sota_wdt_is_armed(void) {
    return s_armed;
}
