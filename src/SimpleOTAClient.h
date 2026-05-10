// SimpleOTAClient Arduino client library
// Targets: ESP32 (Arduino-ESP32 core).
// See README.md for the build-number / NVS rule. It is load-bearing.

#ifndef SIMPLEOTACLIENT_H
#define SIMPLEOTACLIENT_H

#include <Arduino.h>
#include <stdint.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Compile-time configuration. Override by -D or by #define before #include.
// ---------------------------------------------------------------------------

#ifndef SIMPLEOTA_CHECK_INTERVAL_S
// Informational only. The library does not schedule checks; this constant is
// exposed for examples / user code that polls in loop().
#define SIMPLEOTA_CHECK_INTERVAL_S 3600
#endif

#ifndef SIMPLEOTA_TIMEOUT_MS
// HTTP transport timeout for /check and /status requests.
#define SIMPLEOTA_TIMEOUT_MS 15000
#endif

#ifndef SIMPLEOTA_DEBUG
// 0 = silent (default). 1 = verbose Serial logging with [SimpleOTAClient] prefix.
#define SIMPLEOTA_DEBUG 0
#endif

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

enum OTAResult {
    OTA_SUCCESS = 0,        // firmware flashed and validated; reboot pending
    OTA_CHECKSUM_FAIL = 1,  // downloaded payload did not match expected SHA-256
    OTA_FLASH_FAIL = 2,     // network or partition write failure
    OTA_NO_OFFER = 3        // apply() called without a successful preceding check()
};

class SimpleOTAClient {
public:
    // token:   project token or device token, sent as "Authorization: Bearer <token>"
    // chipFamily: "esp32", "esp32s3", ... (use CHIP_xxx constants below)
    // deviceId: stable per-unit identifier. Pass nullptr (default) to use the
    //   Wi-Fi MAC address, formatted as "aa:bb:cc:dd:ee:ff". Supply your own
    //   string if you need a different identifier scheme.
    // boardId, hardwareRevision: optional. nullptr -> field omitted.
    SimpleOTAClient(const char* token,
                    const char* chipFamily,
                    const char* deviceId         = nullptr,
                    const char* boardId          = nullptr,
                    const char* hardwareRevision = nullptr);

    // Polls /api/v1/ota/check/. Returns true if an update is offered and
    // stores the offer (url, checksum, size, deployment_id, build_number)
    // on the instance. Returns false on no-update or on transport failure.
    bool check();

    // Executes the pending offer from the most recent successful check().
    // Reports lifecycle events to /api/v1/ota/status/. On success, persists
    // the new build number into NVS and (by default) calls esp_restart().
    OTAResult apply();

    // Manually post a status event. Returns true on HTTP 2xx with
    // "accepted": true. Requires a deployment context populated by a
    // successful check(). The context is retained through apply() so
    // post-apply events (e.g. report("rebooted")) work, and is cleared on
    // the next check() invocation. Returns false outside that window.
    bool report(const char* event, const char* reason = nullptr);

    // Override the CA certificate (PEM) used for HTTPS verification. By
    // default the library uses the bundled kSimpleOtaRootCA (ISRG Root X1),
    // which covers both the SimpleOTA API and its firmware storage. Call
    // this only if you need to pin a different certificate. Pass nullptr
    // to disable TLS verification entirely (development only; emits a
    // one-shot Serial warning).
    void setCACert(const char* pemRootCA);

    // If true (default), apply() calls esp_restart() after a successful flash.
    // If false, apply() returns OTA_SUCCESS and the caller is responsible for
    // rebooting at a convenient time. Note: the "validated" status event is
    // reported before reboot in either case.
    void setAutoReboot(bool enabled);

    // Partition profile reported to the server (e.g. "default_4mb",
    // "minimal_spiffs_4mb"). Required when your SimpleOTA artifacts are tagged
    // with a partition_profile; the server uses it to filter hardware-compatible
    // builds. nullptr (default) = field omitted from the request.
    void setPartitionProfile(const char* profile);

    // NVS schema version (default: 1). Increment when you restructure your own
    // NVS namespace layout so the server can gate builds that require the new
    // schema. The server assumes 1 when the field is absent; this library always
    // sends it explicitly.
    void setNvsSchemaVersion(uint8_t version);

    // Arbitrary key/value metadata for server-side deployment targeting.
    // Must be a valid JSON object literal, e.g. "{\"site\":\"factory-A\"}".
    // The string is embedded verbatim in the request body; no escaping is done.
    // nullptr (default) = field omitted.
    void setLabels(const char* jsonObject);

    // Security mode advertised to the server (e.g. "basic"). Used for
    // server-side compatibility filtering. nullptr (default) = field omitted.
    void setSecurityMode(const char* mode);

    // Human-readable version label of the firmware currently running
    // (e.g. "1.4.1"). Sent as version_label and shown on the dashboard under
    // Reported firmware state → Version label. Display only; not used for
    // compatibility checks.
    // When nullptr (default), the library reads the value persisted in NVS by
    // the last successful apply(), so post-OTA boots report the correct label
    // automatically without any code change in the new firmware image.
    void setVersionLabel(const char* label);

    // Release channel this device subscribes to (e.g. "stable", "beta").
    // Only applied by the server on first registration or when the device has
    // no channel yet. nullptr (default) = field omitted.
    void setChannel(const char* channel);

    // Version label of the last update offered by check() (e.g. "1.4.2").
    // Valid between a successful check() and the apply() that consumes the
    // offer. Returns an empty string before any offer has been received, and
    // after apply() clears the offer.
    const char* lastOfferedVersion() const;

    // Enable/disable verbose [SimpleOTAClient] logging on Serial at runtime.
    // Equivalent to compiling with -DSIMPLEOTA_DEBUG=1, but settable from
    // the sketch (a `#define SIMPLEOTA_DEBUG 1` in the .ino does NOT affect
    // the library's translation unit). Default: off, unless the library
    // was compiled with -DSIMPLEOTA_DEBUG=1, in which case it defaults on.
    static void setDebug(bool enabled);

    // Managed mode: starts a FreeRTOS background task that periodically calls
    // check() and apply().
    //
    // checkIntervalSec: how often to check (default: SIMPLEOTA_CHECK_INTERVAL_S).
    // onResult: optional callback invoked with the OTAResult after apply()
    //   returns. Not called when apply() triggers an automatic reboot.
    // isConnected: optional callback returning true when IP connectivity is
    //   available. The task polls this every second and only attempts a check
    //   when it returns true. If null (default), the task always attempts a
    //   check at each interval; transport failures are absorbed by the normal
    //   retry/return-false paths.
    //
    // The library is transport-agnostic: it does not import any networking
    // stack. The application owns Wi-Fi/Ethernet/PPP/etc.
    //
    // No-op if a task is already running. Safe to call once from setup().
    void begin(uint32_t checkIntervalSec = SIMPLEOTA_CHECK_INTERVAL_S,
               void (*onResult)(OTAResult) = nullptr,
               bool (*isConnected)()       = nullptr);

    // Stop the background task started by begin(). Do not call while a
    // firmware flash is in progress.
    void end();

    // PEM bundle covering simpleota.com and the pre-signed object-storage
    // host. Pass to setCACert() to opt into verified TLS.
    static const char* kSimpleOtaRootCA;

    // Chip family constants for the chipFamily constructor parameter.
    // Using these avoids typos and provides IDE autocomplete. A raw string
    // literal is also accepted for chips not listed here.
    static const char* const CHIP_ESP32;       // "esp32"
    static const char* const CHIP_ESP32S2;     // "esp32s2"
    static const char* const CHIP_ESP32S3;     // "esp32s3"
    static const char* const CHIP_ESP32C3;     // "esp32c3"
    static const char* const CHIP_ESP32C6;     // "esp32c6"
    static const char* const CHIP_ESP32H2;     // "esp32h2"

    // Security mode constants for setSecurityMode().
    // Raw string literals are also accepted for forward compatibility.
    static const char* const SECURITY_MODE_BASIC;   // "basic"  - HTTPS + SHA-256 checksum (default)
    static const char* const SECURITY_MODE_TOKEN;   // "token"  - per-device token auth (partial, see wiki)
    static const char* const SECURITY_MODE_SIGNED;  // "signed" - firmware signature verification (coming soon)

    // Runtime gate for SOTA_LOG. Public so the logging macro in the .cpp
    // can read it without a friend declaration. Don't write to it directly;
    // use setDebug().
    static bool _debugEnabled;

private:
    // Configuration (not owned; caller must keep strings valid).
    const char* _token;
    const char* _deviceId;
    char        _deviceIdBuf[18];  // storage for auto-generated MAC address
    const char* _chipFamily;
    const char* _boardId;
    const char* _hwRev;
    const char* _partitionProfile;
    uint8_t     _nvsSchemaVersion;
    const char* _labels;       // pre-formed JSON object string, or nullptr
    const char* _securityMode;
    const char* _versionLabel;   // nullptr -> read from NVS
    const char* _channel;        // nullptr -> field omitted
    const char* _caCert;         // nullptr -> setInsecure()
    bool _autoReboot;

    // Pending offer from check(). _hasOffer guards apply().
    bool _hasOffer;
    String _url;
    String _checksum;          // lowercase hex sha256
    uint32_t _size;
    String _deploymentId;
    uint32_t _buildNumber;
    String _offeredVersion;    // version label from last check() offer

    // Internal helpers (defined in .cpp).
    bool _insecureWarned;
    int  postJson(const char* path, const String& body, String* outResp);
    bool sendStatus(const char* event, const char* reason);
    uint32_t readBuildNumber();
    void writeBuildNumber(uint32_t n);
    String readHashFromNvs();
    void   writeHashToNvs(const char* hex);
    String readVersionFromNvs();
    void   writeVersionToNvs(const char* version);
    void clearOffer();
    void warnInsecureOnce();

    // Managed-mode state.
    uint32_t     _checkIntervalSec;
    void       (*_onResult)(OTAResult);
    bool       (*_isConnected)();
    TaskHandle_t _taskHandle;
    static void  _taskEntry(void* arg);
    void         _taskLoop();
};

#endif  // SIMPLEOTACLIENT_H
