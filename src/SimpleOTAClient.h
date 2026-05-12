/**
 * @file SimpleOTAClient.h
 * @brief SimpleOTAClient Arduino client library for ESP32 (Arduino-ESP32 core).
 *
 * See README.md for the build-number / NVS rule. It is load-bearing.
 */

#ifndef SIMPLEOTACLIENT_H
#define SIMPLEOTACLIENT_H

#include <Arduino.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

// ---------------------------------------------------------------------------
// Compile-time configuration. Override with -D or a #define before #include.
// ---------------------------------------------------------------------------

#ifndef SIMPLEOTA_CHECK_INTERVAL_S
/**
 * @brief Default check interval in seconds (informational only).
 *
 * The library does not schedule checks internally; this constant is exposed
 * for examples and user code that polls in loop().
 */
#define SIMPLEOTA_CHECK_INTERVAL_S 3600
#endif

#ifndef SIMPLEOTA_TIMEOUT_MS
/**
 * @brief HTTP transport timeout in milliseconds for /check and /status requests.
 */
#define SIMPLEOTA_TIMEOUT_MS 15000
#endif

#ifndef SIMPLEOTA_DEBUG
/**
 * @brief Verbose logging switch. 0 = silent (default); 1 = verbose Serial
 *        output with a [SimpleOTAClient] prefix.
 */
#define SIMPLEOTA_DEBUG 0
#endif

#ifndef SIMPLEOTA_CONFIRM_TIMEOUT_S
/**
 * @brief Default trial-install confirm deadline in seconds.
 *
 * After a successful apply(), the new firmware must call confirmRunning()
 * within this many seconds, or the library reboots into the previous
 * partition. See setConfirmTimeout().
 */
#define SIMPLEOTA_CONFIRM_TIMEOUT_S 300
#endif

#ifndef SIMPLEOTA_TRIAL_RETRY_INTERVAL_S
/**
 * @brief check() retry interval in seconds during a TRIAL boot (managed mode).
 *
 * Instead of the normal checkIntervalSec, the managed task retries on this
 * shorter interval until a successful server response is received or the
 * confirm timeout expires.
 */
#define SIMPLEOTA_TRIAL_RETRY_INTERVAL_S 10
#endif

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/**
 * @brief Result codes returned by apply().
 */
enum OTAResult {
    OTA_SUCCESS       = 0,  ///< Firmware flashed and validated; reboot pending.
    OTA_CHECKSUM_FAIL = 1,  ///< Downloaded payload did not match expected SHA-256.
    OTA_FLASH_FAIL    = 2,  ///< Network or partition write failure.
    OTA_NO_OFFER      = 3   ///< apply() called without a successful preceding check().
};

/**
 * @class SimpleOTAClient
 * @brief Over-the-air firmware update client for ESP32.
 *
 * Provides check/apply firmware update mechanics, optional lifecycle event
 * reporting, and a trial-install/rollback subsystem (v0.2.0).
 */
class SimpleOTAClient {
public:

    /**
     * @brief Construct a SimpleOTAClient.
     *
     * @param token             Project or device token, sent as
     *                          "Authorization: Bearer <token>".
     * @param chipFamily        Chip family string, e.g. "esp32" or "esp32s3".
     *                          Use the CHIP_xxx constants for IDE autocomplete.
     * @param deviceId          Stable per-unit identifier. Pass nullptr (default)
     *                          to auto-derive from the Wi-Fi MAC address
     *                          ("aa:bb:cc:dd:ee:ff"). Supply your own string for
     *                          a different identifier scheme.
     * @param boardId           Optional board identifier. nullptr = field omitted.
     * @param hardwareRevision  Optional hardware revision. nullptr = field omitted.
     */
    SimpleOTAClient(const char* token,
                    const char* chipFamily,
                    const char* deviceId         = nullptr,
                    const char* boardId          = nullptr,
                    const char* hardwareRevision = nullptr);

    /**
     * @brief Poll /api/v1/ota/check/ for a firmware update.
     *
     * Stores the offer (url, checksum, size, deployment_id, build_number) on
     * the instance when one is available.
     *
     * @return true if an update is offered; false on no-update or transport failure.
     */
    bool check();

    /**
     * @brief Execute the pending offer from the most recent successful check().
     *
     * Reports lifecycle events to /api/v1/ota/status/. On success, persists the
     * new build number in NVS and (by default) calls esp_restart().
     *
     * @return An OTAResult code. On OTA_SUCCESS with auto-reboot enabled
     *         (default), this function does not return.
     */
    OTAResult apply();

    /**
     * @brief Manually post a status event to /api/v1/ota/status/.
     *
     * Requires a deployment context populated by a successful check(). The
     * context is retained through apply() for post-apply events (e.g.
     * report("reboot") when auto-reboot is disabled), and is cleared on the
     * next check() invocation.
     *
     * @param event   Event name string (e.g. "reboot").
     * @param reason  Optional reason string. nullptr = field omitted.
     * @return true on HTTP 2xx with "accepted": true; false otherwise, or if
     *         no deployment context is available.
     */
    bool report(const char* event, const char* reason = nullptr);

    /**
     * @brief Override the CA certificate (PEM) used for HTTPS verification.
     *
     * By default the library uses the bundled kSimpleOtaRootCA (ISRG Root X1),
     * which covers both the SimpleOTA API and its firmware storage. Call this
     * only if you need to pin a different certificate.
     *
     * @param pemRootCA  PEM-encoded root CA certificate. Pass nullptr to disable
     *                   TLS verification entirely (development only; emits a
     *                   one-shot Serial warning).
     */
    void setCACert(const char* pemRootCA);

    /**
     * @brief Control whether apply() automatically reboots after a successful flash.
     *
     * When true (default), apply() calls esp_restart() after a successful flash.
     * When false, apply() returns OTA_SUCCESS and the caller is responsible for
     * rebooting at a convenient time. The "validated" status event is reported
     * before reboot in either case.
     *
     * @param enabled  true = auto-reboot (default); false = caller-managed reboot.
     */
    void setAutoReboot(bool enabled);

    /**
     * @brief Emit a "reboot" status event and restart the device.
     *
     * Convenience helper for applications using setAutoReboot(false). Posts
     * `event="reboot"` to /api/v1/ota/status/ for the just-applied deployment
     * (best-effort; the function does not block on network failure beyond the
     * normal HTTPClient timeout), then calls esp_restart(). Does not return.
     *
     * Must be called in the post-apply window: between a successful apply()
     * and the next check(). Outside that window the deployment context has
     * been cleared and the status POST is skipped, but the device still
     * reboots.
     */
    void rebootForUpdate();

    /**
     * @brief Set the partition profile reported to the server.
     *
     * Required when your SimpleOTA artifacts are tagged with a partition_profile;
     * the server uses it to filter hardware-compatible builds.
     * Example values: "default_4mb", "minimal_spiffs_4mb".
     *
     * @param profile  Partition profile string. nullptr (default) = field omitted.
     */
    void setPartitionProfile(const char* profile);

    /**
     * @brief Set the NVS schema version reported to the server.
     *
     * Increment when you restructure your own NVS namespace layout so the server
     * can gate builds that require the new schema. The server assumes version 1
     * when the field is absent; this library always sends it explicitly.
     *
     * @param version  Schema version number (default: 1).
     */
    void setNvsSchemaVersion(uint8_t version);

    /**
     * @brief Set arbitrary key/value metadata for server-side deployment targeting.
     *
     * @param jsonObject  A valid JSON object literal, e.g. "{\"site\":\"factory-A\"}".
     *                    The string is embedded verbatim in the request body; no
     *                    escaping is performed. nullptr (default) = field omitted.
     */
    void setLabels(const char* jsonObject);

    /**
     * @brief Set the security mode advertised to the server.
     *
     * Used for server-side compatibility filtering.
     *
     * @param mode  Security mode string, e.g. "basic". Use the SECURITY_MODE_xxx
     *              constants for known values. nullptr (default) = field omitted.
     */
    void setSecurityMode(const char* mode);

    /**
     * @brief Set a human-readable version label for the currently-running firmware.
     *
     * Sent as version_label and shown on the dashboard under
     * "Reported firmware state > Version label". Display only; not used for
     * compatibility checks.
     *
     * When nullptr (default), the library reads the value persisted in NVS by the
     * last successful apply(), so post-OTA boots report the correct label
     * automatically without any code change in the new firmware image.
     *
     * @param label  Version label string, e.g. "1.4.1". nullptr = read from NVS.
     */
    void setVersionLabel(const char* label);

    /**
     * @brief Set the release channel this device subscribes to.
     *
     * Only applied by the server on first registration or when the device has no
     * channel yet.
     *
     * @param channel  Channel name, e.g. "stable" or "beta".
     *                 nullptr (default) = field omitted.
     */
    void setChannel(const char* channel);

    /**
     * @brief Return the version label from the last check() offer.
     *
     * Valid between a successful check() and the apply() that consumes the offer.
     *
     * @return Offered version label string, e.g. "1.4.2". Returns an empty string
     *         before any offer has been received, or after apply() clears the offer.
     */
    const char* lastOfferedVersion() const;

    // ------------------------------------------------------------------
    // Trial-install / rollback API (v0.2.0)
    //
    // After a successful apply() that reboots into a new image, the device
    // enters a "trial" state. The new firmware must call confirmRunning()
    // within setConfirmTimeout() seconds, or the library reboots into the
    // previous partition and reports a "rolled_back" event to the server on
    // the next successful network round-trip.
    //
    // In managed mode (begin()), the library auto-calls confirmRunning() on
    // the first HTTP 2xx from /check/ unless setManagedAutoConfirm(false)
    // was called. In polling mode (manual check()/apply()), the application
    // is responsible for calling confirmRunning() once it determines the new
    // image is healthy.
    //
    // Limitation: an image that crashes before the library's boot-validation
    // code runs (e.g. in a global constructor or during Serial bring-up) will
    // reboot-loop into the bad image. See README for details.
    // ------------------------------------------------------------------

    /**
     * @brief Master switch for the trial-install / rollback machinery.
     *
     * Default: true.
     *
     * When set to false, apply() behaves as it did pre-0.2.0: write the new
     * image, reboot, and never look back. A device already in a TRIAL boot
     * from a previous apply() will still complete that trial regardless of
     * this flag, to avoid stranding fleets when the setting is changed.
     *
     * @param enabled  true = rollback enabled (default); false = no trial state.
     */
    void setRollbackEnabled(bool enabled);

    /**
     * @brief Control whether managed mode auto-confirms on a successful /check/ response.
     *
     * Managed mode only. Default: true.
     *
     * When true, the managed task auto-calls confirmRunning() on the first HTTP
     * 2xx from /check/ during a TRIAL boot. When false, the application must call
     * confirmRunning() itself (e.g. from the onResult callback or after its own
     * health checks complete).
     *
     * @param enabled  true = auto-confirm (default); false = manual confirm required.
     */
    void setManagedAutoConfirm(bool enabled);

    /**
     * @brief Set the trial-install confirm deadline.
     *
     * Default: SIMPLEOTA_CONFIRM_TIMEOUT_S (300 seconds). Clamped to [10, 86400].
     *
     * Must exceed your worst-case boot-to-connect-to-first-server-response time.
     * For cellular/PPP fleets where modem registration can take 60+ seconds,
     * increase this accordingly (e.g. setConfirmTimeout(600)).
     *
     * Call this before begin() in managed mode, or before the first check() in
     * polling mode, on the trial boot. Changing it after the timer is armed has
     * no effect on the current trial.
     *
     * @param seconds  Confirm deadline in seconds. Clamped to [10, 86400].
     */
    void setConfirmTimeout(uint32_t seconds);

    /**
     * @brief Confirm that the currently-running firmware is healthy.
     *
     * Cancels the rollback timer and clears trial state from NVS so subsequent
     * boots are normal. Safe to call unconditionally from setup(), even before
     * check() or begin().
     *
     * @return true if a trial was active and is now confirmed; false if the device
     *         was not in a trial (the call is a no-op in that case).
     */
    bool confirmRunning();

    /**
     * @brief Query whether this boot is a not-yet-confirmed trial install.
     *
     * Returns true if this boot is running a new, unconfirmed image and the
     * rollback timer is still armed. Useful for status displays or to delay
     * non-essential application work until the trial resolves. Safe to call
     * before check() or begin().
     *
     * @return true if a trial is in progress; false otherwise.
     */
    bool isTrialInstall();

    /**
     * @brief Enable or disable verbose Serial logging at runtime.
     *
     * Equivalent to compiling with -DSIMPLEOTA_DEBUG=1, but settable from the
     * sketch. Note: a "#define SIMPLEOTA_DEBUG 1" in the .ino does NOT affect
     * the library's translation unit. Default: off, unless the library was
     * compiled with -DSIMPLEOTA_DEBUG=1.
     *
     * @param enabled  true = verbose logging on; false = silent.
     */
    static void setDebug(bool enabled);

    /**
     * @brief Start the managed-mode background task.
     *
     * Starts a FreeRTOS task that periodically calls check() and apply(). The
     * library is transport-agnostic: it does not manage Wi-Fi, Ethernet, or PPP.
     * The application owns connectivity.
     *
     * No-op if a task is already running. Safe to call once from setup().
     *
     * @param checkIntervalSec  How often to check for updates, in seconds.
     *                          Default: SIMPLEOTA_CHECK_INTERVAL_S.
     * @param onResult          Optional callback invoked with the OTAResult after
     *                          apply() returns. Not called when apply() triggers
     *                          an automatic reboot.
     * @param isConnected       Optional callback returning true when IP connectivity
     *                          is available. The task polls this every second and
     *                          only attempts a check when it returns true. If nullptr
     *                          (default), the task always attempts a check; transport
     *                          failures are absorbed by the normal retry paths.
     */
    void begin(uint32_t checkIntervalSec = SIMPLEOTA_CHECK_INTERVAL_S,
               void (*onResult)(OTAResult) = nullptr,
               bool (*isConnected)()       = nullptr);

    /**
     * @brief Stop the managed-mode background task started by begin().
     *
     * @warning Do not call while a firmware flash is in progress.
     */
    void end();

    /**
     * @brief PEM bundle covering simpleota.com and its pre-signed object-storage host.
     *
     * Pass to setCACert() to opt into verified TLS.
     */
    static const char* kSimpleOtaRootCA;

    /// @name Chip family constants
    /// Pass to the chipFamily constructor parameter. Using these avoids typos
    /// and provides IDE autocomplete. A raw string literal is also accepted
    /// for chips not listed here.
    /// @{
    static const char* const CHIP_ESP32;    ///< "esp32"
    static const char* const CHIP_ESP32S2;  ///< "esp32s2"
    static const char* const CHIP_ESP32S3;  ///< "esp32s3"
    static const char* const CHIP_ESP32C3;  ///< "esp32c3"
    static const char* const CHIP_ESP32C6;  ///< "esp32c6"
    static const char* const CHIP_ESP32H2;  ///< "esp32h2"
    /// @}

    /// @name Security mode constants
    /// Pass to setSecurityMode(). Raw string literals are also accepted for
    /// forward compatibility.
    /// @{
    static const char* const SECURITY_MODE_BASIC;   ///< "basic"  - HTTPS + SHA-256 checksum (default).
    static const char* const SECURITY_MODE_TOKEN;   ///< "token"  - per-device token auth (partial; see wiki).
    static const char* const SECURITY_MODE_SIGNED;  ///< "signed" - firmware signature verification (coming soon).
    /// @}

    /**
     * @brief Runtime gate for SOTA_LOG.
     *
     * Public so the logging macro in the .cpp can read it without a friend
     * declaration. Do not write to it directly; use setDebug().
     */
    static bool _debugEnabled;

private:
    // Configuration (not owned; caller must keep strings valid).
    const char* _token;
    const char* _deviceId;
    char        _deviceIdBuf[18];  ///< Storage for the auto-generated MAC address.
    const char* _chipFamily;
    const char* _boardId;
    const char* _hwRev;
    const char* _partitionProfile;
    uint8_t     _nvsSchemaVersion;
    const char* _labels;           ///< Pre-formed JSON object string, or nullptr.
    const char* _securityMode;
    const char* _versionLabel;     ///< nullptr = read from NVS.
    const char* _channel;          ///< nullptr = field omitted.
    const char* _caCert;           ///< nullptr = setInsecure().
    bool        _autoReboot;

    // Pending offer from check(). _hasOffer guards apply().
    bool     _hasOffer;
    String   _url;
    String   _checksum;            ///< Lowercase hex SHA-256.
    uint32_t _size;
    String   _deploymentId;
    uint32_t _buildNumber;
    String   _offeredVersion;      ///< Version label from the last check() offer.

    // Trial-install / rollback state.
    bool          _rollbackEnabled;
    bool          _managedAutoConfirm;
    uint32_t      _confirmTimeoutSec;
    bool          _bootValidated;      ///< processBootValidation() has run this boot.
    bool          _inTrial;            ///< This boot is an unconfirmed new image.
    bool          _rolledBackPending;  ///< trial=2 on boot; rolled_back report deferred.
    bool          _confirmedPending;   ///< Deferred "confirmed" report queued for next /check/.
    String        _confirmedDep;       ///< deployment_id for the deferred "confirmed" POST.
    uint32_t      _confirmedBuild;     ///< build_number for the deferred "confirmed" POST.
    TimerHandle_t _confirmTimer;
    bool          _lastCheckOk;        ///< Set true inside check() on any 2xx from /check/.
    static void   _confirmTimerCb(TimerHandle_t t);

    // Internal helpers (defined in .cpp).
    bool _insecureWarned;
    bool _authWarned;   ///< One-shot warning emitted on the first 401/403 from the server.
    int  postJson(const char* path, const String& body, String* outResp);
    bool sendStatus(const char* event, const char* reason);
    bool sendStatusFor(const char* event, const char* reason,
                       const char* deploymentId, uint32_t buildNumber);
    uint32_t readBuildNumber();
    void     writeBuildNumber(uint32_t n);
    String   readHashFromNvs();
    void     writeHashToNvs(const char* hex);
    String   readVersionFromNvs();
    void     writeVersionToNvs(const char* version);
    void     clearOffer();
    void     warnInsecureOnce();

    // Rollback internals.
    void processBootValidation();
    void snapshotPreOtaState();        ///< Called from apply() before esp_restart().
    void performRollback();            ///< Timer callback target and explicit caller.
    void clearTrialState();            ///< Wipe all sota_prev_* / sota_fail_* / sota_trial / sota_conf_pend keys.
    void reportRolledBackIfPending();  ///< Attempt the deferred rolled_back POST.
    void reportConfirmedIfPending();   ///< Attempt the deferred confirmed POST.

    // Managed-mode state.
    uint32_t     _checkIntervalSec;
    void       (*_onResult)(OTAResult);
    bool       (*_isConnected)();
    TaskHandle_t _taskHandle;
    static void  _taskEntry(void* arg);
    void         _taskLoop();
    void         _trialLoop();  ///< Managed-mode loop while _inTrial.
};

#endif  // SIMPLEOTACLIENT_H
