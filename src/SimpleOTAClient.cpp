// SimpleOTAClient Arduino client library: implementation.

#include "SimpleOTAClient.h"

// On Arduino-ESP32 3.x the TLS client class was renamed from WiFiClientSecure
// to NetworkClientSecure to reflect that it is transport-agnostic (works over
// Wi-Fi, Ethernet, PPP). On 2.x only WiFiClientSecure exists. The class is a
// generic TLS-over-TCP client in both cores; the names are otherwise
// equivalent.
#if __has_include(<NetworkClientSecure.h>)
  #include <NetworkClientSecure.h>
  using SimpleOtaTLSClient = NetworkClientSecure;
#else
  #include <WiFiClientSecure.h>
  using SimpleOtaTLSClient = WiFiClientSecure;
#endif
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Tolerate `#define SIMPLEOTA_DEBUG` (no value) by treating any defined-and-
// truthy macro as enabled. The compile-time flag sets the default; setDebug()
// can flip it at runtime. The runtime check costs one global load per call
// site, which is negligible compared to the cost of the Serial.printf.
#if defined(SIMPLEOTA_DEBUG) && (SIMPLEOTA_DEBUG + 0)
bool SimpleOTAClient::_debugEnabled = true;
#else
bool SimpleOTAClient::_debugEnabled = false;
#endif

#define SOTA_LOG(fmt, ...) do { \
    if (SimpleOTAClient::_debugEnabled) \
        Serial.printf("[SimpleOTAClient] " fmt "\n", ##__VA_ARGS__); \
} while (0)

// ---------------------------------------------------------------------------
// NVS (build-number persistence)
// ---------------------------------------------------------------------------

static const char* kNvsNamespace = "simpleota";
static const char* kNvsKeyBuild  = "sota_build";
static const char* kNvsKeyHash    = "sota_hash";
static const char* kNvsKeyVersion = "sota_ver";

// Trial-install / rollback NVS keys. See SimpleOTAClient.h for the lifecycle.
// All names fit within the Preferences 15-char limit.
static const char* kNvsKeyTrial       = "sota_trial";       // uint8: 0/1/2
static const char* kNvsKeyPrevPart    = "sota_prev_part";   // uint32 partition addr
static const char* kNvsKeyPrevBuild   = "sota_prev_build";  // uint32
static const char* kNvsKeyPrevHash    = "sota_prev_hash";   // string
static const char* kNvsKeyPrevVer     = "sota_prev_ver";    // string
static const char* kNvsKeyFailDep     = "sota_fail_dep";    // string
static const char* kNvsKeyFailBuild   = "sota_fail_build";  // uint32
static const char* kNvsKeyConfirmPend = "sota_conf_pend";   // uint8: 1 if a "confirmed" report is pending

uint32_t SimpleOTAClient::readBuildNumber() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return 0;
    uint32_t v = p.getUInt(kNvsKeyBuild, 0);
    p.end();
    return v;
}

void SimpleOTAClient::writeBuildNumber(uint32_t n) {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("NVS open failed; build number not persisted");
        return;
    }
    p.putUInt(kNvsKeyBuild, n);
    p.end();
    SOTA_LOG("persisted build_number=%u", (unsigned)n);
}

String SimpleOTAClient::readHashFromNvs() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return "";
    String v = p.getString(kNvsKeyHash, "");
    p.end();
    return v;
}

void SimpleOTAClient::writeHashToNvs(const char* hex) {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("NVS open failed; hash not persisted");
        return;
    }
    p.putString(kNvsKeyHash, hex);
    p.end();
    SOTA_LOG("persisted current_hash");
}

String SimpleOTAClient::readVersionFromNvs() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return "";
    String v = p.getString(kNvsKeyVersion, "");
    p.end();
    return v;
}

void SimpleOTAClient::writeVersionToNvs(const char* version) {
    if (!version || !*version) return;
    // Sanity cap: server-supplied version strings are expected to be short
    // (e.g. "1.4.2"). Reject anything implausibly long rather than spending
    // flash wear cycles on a malformed server response.
    if (strlen(version) > 64) {
        SOTA_LOG("writeVersionToNvs: version string too long (%u), skipping",
                 (unsigned)strlen(version));
        return;
    }
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("NVS open failed; version not persisted");
        return;
    }
    p.putString(kNvsKeyVersion, version);
    p.end();
    SOTA_LOG("persisted version_label=%s", version);
}

// ---------------------------------------------------------------------------
// Tiny JSON helpers, sufficient for the known SimpleOTAClient payload shapes.
// We only look up specific top-level keys; no nested-object support is needed.
// ---------------------------------------------------------------------------

static void appendJsonString(String& out, const char* s) {
    out += '"';
    if (s) {
        for (const char* p = s; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
            else if (c == '\n')          out += "\\n";
            else if (c == '\r')          out += "\\r";
            else if (c == '\t')          out += "\\t";
            else if (c < 0x20)           { /* drop control chars */ }
            else                         out += (char)c;
        }
    }
    out += '"';
}

// Locate the start of the value for a top-level JSON key. Walks the string
// skipping over quoted string contents so that occurrences of the key inside
// a value can't false-match, and requires the matched key to be immediately
// followed (modulo whitespace) by ':', preventing prefix-key collisions
// (e.g. searching "url" matching "signed_url").
static bool jsonFindValue(const String& s, const char* key, int& valStart) {
    size_t klen = strlen(key);
    int n = (int)s.length();
    int p = 0;
    while (p < n) {
        char c = s[p];
        if (c == '"') {
            // Check whether this quoted token is exactly `key`.
            int q = p + 1;
            bool matches = true;
            for (size_t i = 0; i < klen; ++i) {
                if (q + (int)i >= n || s[q + (int)i] != key[i]) { matches = false; break; }
            }
            if (matches && q + (int)klen < n && s[q + (int)klen] == '"') {
                int after = q + (int)klen + 1;
                int look = after;
                while (look < n && (s[look] == ' ' || s[look] == '\t' ||
                                    s[look] == '\n' || s[look] == '\r')) ++look;
                if (look < n && s[look] == ':') {
                    int v = look + 1;
                    while (v < n && (s[v] == ' ' || s[v] == '\t' ||
                                     s[v] == '\n' || s[v] == '\r')) ++v;
                    valStart = v;
                    return true;
                }
            }
            // Not our key; skip the rest of this string, honoring backslash
            // escapes so embedded quotes don't terminate the scan early.
            p = q;
            while (p < n && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < n) p += 2;
                else ++p;
            }
            if (p < n) ++p; // past closing quote
        } else {
            ++p;
        }
    }
    return false;
}

static bool jsonGetString(const String& s, const char* key, String& out) {
    int p;
    if (!jsonFindValue(s, key, p)) return false;
    int n = (int)s.length();
    if (p >= n || s[p] != '"') return false;
    ++p;
    String v;
    v.reserve(64);
    while (p < n && s[p] != '"') {
        char c = s[p];
        if (c == '\\' && p + 1 < n) {
            char esc = s[p + 1];
            if      (esc == 'n') v += '\n';
            else if (esc == 't') v += '\t';
            else if (esc == 'r') v += '\r';
            else                 v += esc;
            p += 2;
        } else {
            v += c;
            ++p;
        }
    }
    out = v;
    return true;
}

static bool jsonGetBool(const String& s, const char* key, bool& out) {
    int p;
    if (!jsonFindValue(s, key, p)) return false;
    if (s.substring(p, p + 4) == "true")  { out = true;  return true; }
    if (s.substring(p, p + 5) == "false") { out = false; return true; }
    return false;
}

static bool jsonGetUint(const String& s, const char* key, uint32_t& out) {
    int p;
    if (!jsonFindValue(s, key, p)) return false;
    int n = (int)s.length();
    uint32_t v = 0;
    bool any = false;
    while (p < n && s[p] >= '0' && s[p] <= '9') {
        v = v * 10u + (uint32_t)(s[p] - '0');
        ++p;
        any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

// ---------------------------------------------------------------------------
// Bundled SimpleOTAClient root CA (ISRG Root X1 / Let's Encrypt). Public PEM,
// distributed widely. Used by default for all HTTPS connections; both the
// SimpleOTA API host and the firmware storage host chain to this root.
// ---------------------------------------------------------------------------

const char* SimpleOTAClient::kSimpleOtaRootCA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
    "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
    "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
    "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
    "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
    "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
    "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
    "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
    "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
    "-----END CERTIFICATE-----\n";

const char* const SimpleOTAClient::CHIP_ESP32   = "esp32";
const char* const SimpleOTAClient::CHIP_ESP32S2 = "esp32s2";
const char* const SimpleOTAClient::CHIP_ESP32S3 = "esp32s3";
const char* const SimpleOTAClient::CHIP_ESP32C3 = "esp32c3";
const char* const SimpleOTAClient::CHIP_ESP32C6 = "esp32c6";
const char* const SimpleOTAClient::CHIP_ESP32H2 = "esp32h2";

const char* const SimpleOTAClient::SECURITY_MODE_BASIC  = "basic";
const char* const SimpleOTAClient::SECURITY_MODE_TOKEN  = "token";
const char* const SimpleOTAClient::SECURITY_MODE_SIGNED = "signed";

// ---------------------------------------------------------------------------
// Construction / configuration
// ---------------------------------------------------------------------------

SimpleOTAClient::SimpleOTAClient(const char* token,
                     const char* chipFamily,
                     const char* deviceId,
                     const char* boardId,
                     const char* hardwareRevision)
    : _token(token),
      _deviceId(nullptr),
      _deviceIdBuf{},
      _chipFamily(chipFamily),
      _boardId(boardId),
      _hwRev(hardwareRevision),
      _partitionProfile(nullptr),
      _nvsSchemaVersion(1),
      _labels(nullptr),
      _securityMode(nullptr),
      _versionLabel(nullptr),
      _channel(nullptr),
      _caCert(SimpleOTAClient::kSimpleOtaRootCA),
      _autoReboot(true),
      _hasOffer(false),
      _size(0),
      _buildNumber(0),
      _rollbackEnabled(true),
      _managedAutoConfirm(true),
      _confirmTimeoutSec(SIMPLEOTA_CONFIRM_TIMEOUT_S),
      _bootValidated(false),
      _inTrial(false),
      _rolledBackPending(false),
      _confirmedPending(false),
      _confirmedBuild(0),
      _confirmTimer(nullptr),
      _lastCheckOk(false),
      _insecureWarned(false),
      _authWarned(false),
      _checkIntervalSec(0),
      _onResult(nullptr),
      _isConnected(nullptr),
      _taskHandle(nullptr) {
    if (deviceId) {
        _deviceId = deviceId;
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(_deviceIdBuf, sizeof(_deviceIdBuf),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        _deviceId = _deviceIdBuf;
    }
}

void SimpleOTAClient::setCACert(const char* pemRootCA)        { _caCert = pemRootCA; }
void SimpleOTAClient::setAutoReboot(bool enabled)             { _autoReboot = enabled; }
void SimpleOTAClient::setPartitionProfile(const char* profile){ _partitionProfile = profile; }
void SimpleOTAClient::setNvsSchemaVersion(uint8_t version)    { _nvsSchemaVersion = version; }
void SimpleOTAClient::setLabels(const char* jsonObject)       { _labels = jsonObject; }
void SimpleOTAClient::setSecurityMode(const char* mode)       { _securityMode = mode; }
void SimpleOTAClient::setVersionLabel(const char* label)      { _versionLabel = label; }
void SimpleOTAClient::setChannel(const char* channel)         { _channel = channel; }
void SimpleOTAClient::setDebug(bool enabled)                  { _debugEnabled = enabled; }
void SimpleOTAClient::setRollbackEnabled(bool enabled)        { _rollbackEnabled = enabled; }
void SimpleOTAClient::setManagedAutoConfirm(bool enabled)     { _managedAutoConfirm = enabled; }
void SimpleOTAClient::setConfirmTimeout(uint32_t seconds) {
    // Clamp to a sane range. Below ~10s gives users no time to debug a TRIAL
    // boot on the serial console; above 24h is a bug, not a configuration.
    if (seconds < 10)    seconds = 10;
    if (seconds > 86400) seconds = 86400;
    _confirmTimeoutSec = seconds;
}
bool SimpleOTAClient::isTrialInstall() {
    processBootValidation();
    return _inTrial;
}
const char* SimpleOTAClient::lastOfferedVersion() const       { return _offeredVersion.c_str(); }

void SimpleOTAClient::begin(uint32_t checkIntervalSec,
                      void (*onResult)(OTAResult),
                      bool (*isConnected)()) {
    if (_taskHandle != nullptr) return;
    _checkIntervalSec = checkIntervalSec;
    _onResult = onResult;
    _isConnected = isConnected;
    // Run boot validation synchronously on the caller (typically the Arduino
    // main task) so the rollback timer is armed before begin() returns. The
    // timer must exist by the time the managed task starts checking _inTrial.
    processBootValidation();
    xTaskCreate(_taskEntry, "simpleota", 8192, this, 1, &_taskHandle);
}

void SimpleOTAClient::end() {
    if (_taskHandle == nullptr) return;
    vTaskDelete(_taskHandle);
    _taskHandle = nullptr;
}

void SimpleOTAClient::_taskEntry(void* arg) {
    static_cast<SimpleOTAClient*>(arg)->_taskLoop();
    vTaskDelete(nullptr);  // should not be reached
}

void SimpleOTAClient::_taskLoop() {
    // While a trial is in progress, run the trial loop (short retry interval,
    // auto-confirm on first 2xx from /check/). Once the trial resolves, fall
    // through to the normal cadence loop. The independent FreeRTOS timer is
    // responsible for actually rebooting on confirm-timeout; _trialLoop()
    // exits when either confirmRunning() was called or the deadline passed.
    if (_inTrial) {
        _trialLoop();
    }

    for (;;) {
        // Wait for IP connectivity if the application provided a probe.
        // Without one, just attempt the check and rely on transport-failure
        // handling to absorb a missing connection.
        if (_isConnected) {
            while (!_isConnected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        if (check()) {
            OTAResult r = apply();  // reboots on success by default
            if (_onResult) _onResult(r);
        }

        vTaskDelay(pdMS_TO_TICKS(_checkIntervalSec * 1000UL));
    }
}

// Managed-mode trial loop: short retry interval, deadline-bounded waits,
// auto-confirm on first 2xx from /check/ (when _managedAutoConfirm is true).
// Exits when the trial is confirmed (by us or by the app) or the deadline
// passes. The FreeRTOS confirm timer will fire performRollback() at the
// deadline regardless of where this loop is currently parked.
void SimpleOTAClient::_trialLoop() {
    const uint32_t deadlineMs = millis() + _confirmTimeoutSec * 1000UL;

    while (_inTrial && (int32_t)(deadlineMs - millis()) > 0) {
        // Wait for IP connectivity if probe supplied. Bounded by the deadline
        // so a forever-disconnected transport does not spin past it.
        if (_isConnected) {
            while (!_isConnected() && (int32_t)(deadlineMs - millis()) > 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if ((int32_t)(deadlineMs - millis()) <= 0) break;

        bool ok = check();
        // _lastCheckOk is set true by check() on any 2xx from /check/,
        // regardless of update_available. This proves boot + connectivity +
        // TLS + token auth + server reachability: the correct trigger for
        // managed auto-confirm.
        if (_managedAutoConfirm && _lastCheckOk) {
            SOTA_LOG("trial: server reachable; auto-confirming (%s)",
                     ok ? "offer available" : "no update");
            confirmRunning();
            if (ok) {
                OTAResult r = apply();
                if (_onResult) _onResult(r);
            }
            return;
        }

        // Bounded sleep before the next attempt.
        uint32_t now = millis();
        if ((int32_t)(deadlineMs - now) <= 0) break;
        uint32_t remaining = deadlineMs - now;
        uint32_t sleep = SIMPLEOTA_TRIAL_RETRY_INTERVAL_S * 1000UL;
        if (sleep > remaining) sleep = remaining;
        vTaskDelay(pdMS_TO_TICKS(sleep));
    }

    // Either confirmRunning() was called externally (drops _inTrial) or the
    // deadline passed. In the latter case the FreeRTOS timer has fired or is
    // about to fire performRollback() -> esp_restart(). Returning here lets
    // the outer task loop continue normally if (somehow) we didn't roll back.
}

// Emit a single Serial warning when running with setInsecure(). This is
// always on (independent of SIMPLEOTA_DEBUG) because shipping firmware that
// flashes server-supplied URLs without TLS verification is the kind of thing
// you want to notice during a first integration run.
void SimpleOTAClient::warnInsecureOnce() {
    if (_insecureWarned || _caCert) return;
    _insecureWarned = true;
    Serial.println(F("[SimpleOTAClient] WARNING: TLS verification disabled. "
                     "Call setCACert(SimpleOTAClient::kSimpleOtaRootCA) for production."));
}

void SimpleOTAClient::clearOffer() {
    _hasOffer       = false;
    _url            = "";
    _checksum       = "";
    _size           = 0;
    _deploymentId   = "";
    _buildNumber    = 0;
    _offeredVersion = "";
}

// ---------------------------------------------------------------------------
// HTTPS POST helper (used for /check and /status only, NOT for the firmware
// download, which streams via HTTPClient::GET in apply()).
//
// Retry policy: on transport failure (HTTP code <= 0), wait 2 s and retry once.
// Any received HTTP response (code > 0) is returned immediately with no retry;
// retrying a 4xx would be pointless and retrying a 5xx is left to the next
// scheduled check() cycle.
// ---------------------------------------------------------------------------

// Reject tokens containing CR/LF; defends against header injection if a
// caller ever sourced the token from untrusted input.
static bool tokenLooksSafe(const char* t) {
    if (!t) return false;
    for (const char* p = t; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

int SimpleOTAClient::postJson(const char* path, const String& body, String* outResp) {
    if (!tokenLooksSafe(_token)) {
        SOTA_LOG("postJson: token contains illegal characters");
        return -1;
    }
    warnInsecureOnce();
    String url = String("https://simpleota.com") + path;
    for (int attempt = 0; attempt < 2; ++attempt) {
        SimpleOtaTLSClient client;
        if (_caCert) client.setCACert(_caCert);
        else         client.setInsecure();

        HTTPClient http;
        http.setTimeout(SIMPLEOTA_TIMEOUT_MS);
        if (!http.begin(client, url)) {
            SOTA_LOG("http.begin failed (attempt %d) url=%s", attempt, url.c_str());
        } else {
            http.addHeader("Authorization", String("Bearer ") + _token);
            http.addHeader("Content-Type", "application/json");
            int code = http.POST(body);
            if (code > 0) {
                if (outResp) *outResp = http.getString();
                http.end();
                SOTA_LOG("POST %s -> %d", path, code);
                if ((code == 401 || code == 403) && !_authWarned) {
                    _authWarned = true;
                    Serial.println(F("[SimpleOTAClient] WARNING: server rejected the token "
                                     "(HTTP 401/403). Check your project token."));
                }
                return code;  // any received response: no retry
            }
            SOTA_LOG("POST %s transport error %d (attempt %d)", path, code, attempt);
            http.end();
        }
        if (attempt == 0) delay(2000);  // only reached on transport failure (code <= 0)
    }
    return -1;
}

// ---------------------------------------------------------------------------
// /api/v1/ota/check/
// ---------------------------------------------------------------------------

bool SimpleOTAClient::check() {
    // Boot validation runs once per process lifetime. Wired into check() so
    // polling-mode users get rollback support without an extra setup() call.
    // begin() also calls this directly so managed mode is symmetric.
    processBootValidation();

    // If a prior boot rolled back, try to inform the server. Best-effort:
    // failure here leaves NVS state intact so we'll retry on the next check().
    reportRolledBackIfPending();

    clearOffer();

    uint32_t currentBuild = readBuildNumber();
    SOTA_LOG("check: current_build_number=%u", (unsigned)currentBuild);

    String body;
    body.reserve(512);
    body += '{';
    body += "\"device_id\":";              appendJsonString(body, _deviceId);
    body += ",\"framework\":\"arduino\"";
    body += ",\"chip_family\":";           appendJsonString(body, _chipFamily);
    if (_boardId) {
        body += ",\"board_id\":";          appendJsonString(body, _boardId);
    }
    if (_hwRev) {
        body += ",\"hardware_revision\":"; appendJsonString(body, _hwRev);
    }
    body += ",\"current_build_number\":";
    body += String(currentBuild);
    body += ",\"nvs_schema_version\":";
    body += String(_nvsSchemaVersion);
    if (_partitionProfile) {
        body += ",\"partition_profile\":"; appendJsonString(body, _partitionProfile);
    }
    if (_labels) {
        // _labels is a pre-formed JSON object string, embedded verbatim.
        body += ",\"labels\":";
        body += _labels;
    }
    if (_securityMode) {
        body += ",\"security_mode\":";     appendJsonString(body, _securityMode);
    }
    {
        String nvVersion;
        const char* vLabel = _versionLabel;
        if (!vLabel) {
            nvVersion = readVersionFromNvs();
            if (nvVersion.length()) vLabel = nvVersion.c_str();
        }
        if (vLabel) {
            body += ",\"version_label\":"; appendJsonString(body, vLabel);
        }
    }
    {
        String nvHash = readHashFromNvs();
        if (nvHash.length()) {
            body += ",\"current_hash\":"; appendJsonString(body, nvHash.c_str());
        }
    }
    if (_channel) {
        body += ",\"channel\":";         appendJsonString(body, _channel);
    }
    body += '}';

    String resp;
    _lastCheckOk = false;
    int code = postJson("/api/v1/ota/check/", body, &resp);
    if (code != 200) {
        SOTA_LOG("check: non-200 (%d), no offer", code);
        return false;
    }
    _lastCheckOk = true;  // any 2xx proves server reachability

    // Now that the server is confirmed reachable, attempt the deferred
    // "confirmed" report for a successful OTA boot. Best-effort: failure
    // leaves NVS state intact so we'll retry on the next check().
    reportConfirmedIfPending();

    bool available = false;
    if (!jsonGetBool(resp, "update_available", available) || !available) {
        SOTA_LOG("check: no update available");
        return false;
    }

    String url, checksum, deploymentId;
    uint32_t buildNumber = 0, size = 0;
    if (!jsonGetString(resp, "url", url) ||
        !jsonGetString(resp, "checksum", checksum) ||
        !jsonGetUint  (resp, "build_number", buildNumber)) {
        SOTA_LOG("check: malformed offer payload");
        return false;
    }
    jsonGetUint  (resp, "size",          size);          // optional
    jsonGetString(resp, "deployment_id", deploymentId);  // optional
    String offeredVersion;
    jsonGetString(resp, "version",       offeredVersion); // optional

    _url            = url;
    _checksum       = checksum;
    _checksum.toLowerCase();
    _size           = size;
    _deploymentId   = deploymentId;
    _buildNumber    = buildNumber;
    _offeredVersion = offeredVersion;
    _hasOffer       = true;

    SOTA_LOG("check: offer build=%u version=%s size=%u deployment=%s",
             (unsigned)buildNumber, offeredVersion.c_str(), (unsigned)size, deploymentId.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// /api/v1/ota/status/
// ---------------------------------------------------------------------------

bool SimpleOTAClient::sendStatusFor(const char* event, const char* reason,
                                    const char* deploymentId, uint32_t buildNumber) {
    // Log the event name + deployment context BEFORE the network call so
    // it's visible even if the POST hangs, and so callers can distinguish
    // back-to-back status posts in the trace (e.g. validated vs. reboot).
    SOTA_LOG("status: event=%s build=%u deployment=%s%s%s",
             event, (unsigned)buildNumber, deploymentId,
             reason ? " reason=" : "", reason ? reason : "");

    String body;
    body.reserve(192);
    body += '{';
    body += "\"device_id\":";     appendJsonString(body, _deviceId);
    body += ",\"deployment_id\":"; appendJsonString(body, deploymentId);
    body += ",\"event\":";         appendJsonString(body, event);
    body += ",\"build_number\":";
    body += String(buildNumber);
    if (reason) {
        body += ",\"reason\":";    appendJsonString(body, reason);
    }
    body += '}';

    String resp;
    int code = postJson("/api/v1/ota/status/", body, &resp);
    if (code < 200 || code >= 300) {
        SOTA_LOG("status: event=%s rejected (http=%d)", event, code);
        return false;
    }
    bool accepted = false;
    jsonGetBool(resp, "accepted", accepted);
    SOTA_LOG("status: event=%s accepted=%d", event, accepted ? 1 : 0);
    return accepted;
}

bool SimpleOTAClient::sendStatus(const char* event, const char* reason) {
    // Convenience wrapper for the common case: the currently-pending offer.
    return sendStatusFor(event, reason, _deploymentId.c_str(), _buildNumber);
}

bool SimpleOTAClient::report(const char* event, const char* reason) {
    // sendStatus() includes deployment_id and build_number; without a
    // deployment ID the backend can't correlate the event. _deploymentId is
    // populated by check() and retained through a successful apply() until
    // the next check() invocation, so report() is valid both while an offer
    // is pending and in the post-apply window (e.g. report("reboot")).
    if (_deploymentId.isEmpty()) {
        SOTA_LOG("report: ignored (no deployment context)");
        return false;
    }
    return sendStatus(event, reason);
}

// Convenience for applications using setAutoReboot(false): emit the same
// "reboot" event apply() would emit on the auto-reboot path, then restart.
// Does not return. Skips the status POST silently if no deployment context
// is available (e.g. called outside the post-apply window) so the reboot
// itself is still honored.
void SimpleOTAClient::rebootForUpdate() {
    if (!_deploymentId.isEmpty()) {
        sendStatus("reboot", nullptr);
    } else {
        SOTA_LOG("rebootForUpdate: no deployment context; skipping status post");
    }
    SOTA_LOG("rebootForUpdate: rebooting");
    delay(100);
    esp_restart();
    // not reached
}

// ---------------------------------------------------------------------------
// apply(): download firmware, hash on the fly, flash, persist build number,
// and (by default) reboot.
// ---------------------------------------------------------------------------

OTAResult SimpleOTAClient::apply() {
    if (!_hasOffer) {
        SOTA_LOG("apply: no offer");
        return OTA_NO_OFFER;
    }

    // Reject non-HTTPS download URLs. The server is expected to issue a
    // pre-signed https:// URL; anything else (especially in the absence of
    // CA pinning) would let an attacker downgrade firmware delivery.
    if (!_url.startsWith("https://")) {
        SOTA_LOG("apply: refusing non-https url");
        sendStatus("failed", "insecure_url");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    warnInsecureOnce();

    sendStatus("download_started", nullptr);

    SimpleOtaTLSClient client;
    if (_caCert) client.setCACert(_caCert);
    else         client.setInsecure();

    HTTPClient http;
    http.setTimeout(SIMPLEOTA_TIMEOUT_MS);
    if (!http.begin(client, _url)) {
        SOTA_LOG("apply: http.begin failed");
        sendStatus("failed", "https_begin_failed");
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char r[32];
        snprintf(r, sizeof(r), "http_status_%d", code);
        SOTA_LOG("apply: GET firmware -> %d", code);
        sendStatus("failed", r);
        http.end();
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    int contentLen = http.getSize();
    size_t expected = (contentLen > 0) ? (size_t)contentLen
                    : (_size > 0)      ? (size_t)_size
                                       : (size_t)UPDATE_SIZE_UNKNOWN;

    if (!Update.begin(expected)) {
        SOTA_LOG("apply: Update.begin failed (need=%u)", (unsigned)expected);
        sendStatus("failed", "update_begin_failed");
        http.end();
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, /*is224=*/0);

    // getStreamPtr() returns WiFiClient* on Arduino-ESP32 2.x and
    // NetworkClient* on 3.x; auto* keeps us source-compatible with both.
    auto* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t total = 0;
    uint32_t lastByteAt = millis();

    while (http.connected() && (contentLen < 0 || total < (size_t)contentLen)) {
        int avail = stream->available();
        if (avail < 0) {
            // Negative return == socket error on Arduino-ESP32.
            mbedtls_sha256_free(&sha);
            Update.abort();
            SOTA_LOG("apply: stream error during download at %u bytes", (unsigned)total);
            sendStatus("failed", "stream_error");
            http.end();
            clearOffer();
            return OTA_FLASH_FAIL;
        }
        if (avail > 0) {
            size_t want = ((size_t)avail < sizeof(buf)) ? (size_t)avail : sizeof(buf);
            int n = stream->readBytes(buf, want);
            if (n <= 0) break;
            if (Update.write(buf, n) != (size_t)n) {
                mbedtls_sha256_free(&sha);
                Update.abort();
                sendStatus("failed", "update_write_failed");
                http.end();
                clearOffer();
                return OTA_FLASH_FAIL;
            }
            mbedtls_sha256_update(&sha, buf, n);
            total += (size_t)n;
            lastByteAt = millis();
            // Keep the IDLE task fed on fast links so the task watchdog
            // doesn't trip during long sustained downloads.
            yield();
        } else {
            if (millis() - lastByteAt > SIMPLEOTA_TIMEOUT_MS) {
                mbedtls_sha256_free(&sha);
                Update.abort();
                SOTA_LOG("apply: download stalled at %u bytes", (unsigned)total);
                sendStatus("failed", "download_stalled");
                http.end();
                clearOffer();
                return OTA_FLASH_FAIL;
            }
            delay(1);
        }
    }

    if (contentLen > 0 && total != (size_t)contentLen) {
        mbedtls_sha256_free(&sha);
        Update.abort();
        SOTA_LOG("apply: short read %u/%d", (unsigned)total, contentLen);
        sendStatus("failed", "short_read");
        http.end();
        clearOffer();
        return OTA_FLASH_FAIL;
    }

    sendStatus("downloaded", nullptr);

    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    char hex[65];
    for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = 0;

    if (_checksum != hex) {
        SOTA_LOG("apply: checksum mismatch expected=%s actual=%s",
                 _checksum.c_str(), hex);
        Update.abort();
        sendStatus("failed", "checksum_mismatch");
        http.end();
        clearOffer();
        return OTA_CHECKSUM_FAIL;
    }

    if (!Update.end(true)) {
        SOTA_LOG("apply: Update.end failed err=%d", (int)Update.getError());
        sendStatus("failed", "update_end_failed");
        http.end();
        clearOffer();
        return OTA_FLASH_FAIL;
    }
    http.end();

    sendStatus("flashed", nullptr);

    // Snapshot the OUTGOING image's identity before we overwrite NVS, so a
    // post-apply rollback can restore it. Reads from NVS read what was just
    // written by the PREVIOUS apply() (or the factory defaults). Must happen
    // before the writeBuildNumber/writeHashToNvs/writeVersionToNvs calls.
    // Gated on _rollbackEnabled so users who opted out pay no NVS cost.
    if (_rollbackEnabled) {
        snapshotPreOtaState();
    }

    writeBuildNumber(_buildNumber);
    writeHashToNvs(hex);
    writeVersionToNvs(_offeredVersion.c_str());
    sendStatus("validated", nullptr);

    // Persist deployment context + confirm-pending flag UNCONDITIONALLY so
    // the next boot can emit a "confirmed" report regardless of rollback
    // mode. When rollback is enabled, also arm the trial flag in the same
    // NVS commit so a power loss between writes cannot leave the device
    // half-armed. processBootValidation() on the next boot reads these to
    // decide what to do.
    {
        Preferences p;
        if (p.begin(kNvsNamespace, /*readOnly=*/false)) {
            p.putString(kNvsKeyFailDep,     _deploymentId.c_str());
            p.putUInt  (kNvsKeyFailBuild,   _buildNumber);
            p.putUChar (kNvsKeyConfirmPend, 1);
            if (_rollbackEnabled) {
                p.putUChar(kNvsKeyTrial, 1);
                SOTA_LOG("apply: armed trial (timeout=%us deployment=%s)",
                         (unsigned)_confirmTimeoutSec, _deploymentId.c_str());
            } else {
                SOTA_LOG("apply: confirm pending (rollback disabled, deployment=%s)",
                         _deploymentId.c_str());
            }
            p.end();
        } else {
            SOTA_LOG("apply: NVS open failed; confirm/trial state not armed");
        }
    }

    // Clear the download-related fields so a second apply() call (with
    // auto-reboot disabled) doesn't redownload and reflash the same image.
    // _deploymentId and _buildNumber are deliberately retained so the
    // "reboot" event below (and any application report() call) can correlate
    // with the just-applied deployment. The next check() invocation calls
    // clearOffer() at its top, which resets that context.
    _hasOffer       = false;
    _url            = "";
    _checksum       = "";
    _size           = 0;
    _offeredVersion = "";

    if (_autoReboot) {
        // Emit "reboot" immediately before esp_restart() so the server can
        // distinguish a clean apply-driven restart from a crash. Only fires
        // on the auto-reboot path; applications that drive their own restart
        // should call rebootForUpdate() instead of a bare ESP.restart().
        sendStatus("reboot", nullptr);
        SOTA_LOG("apply: rebooting");
        delay(100);
        esp_restart();
        // not reached
    }
    return OTA_SUCCESS;
}

// ---------------------------------------------------------------------------
// Rollback / trial-install machinery (v0.2.0)
// ---------------------------------------------------------------------------

// Snapshot the CURRENT (about-to-be-replaced) image identity into NVS so a
// failed trial can restore it. Called from apply() right before NVS is
// overwritten with the new image's metadata. Reads esp_ota_get_running_-
// partition() for the partition address (not the about-to-boot partition).
void SimpleOTAClient::snapshotPreOtaState() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        SOTA_LOG("snapshot: no running partition; skipping");
        return;
    }
    uint32_t prevBuild = readBuildNumber();
    String   prevHash  = readHashFromNvs();
    String   prevVer   = readVersionFromNvs();

    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("snapshot: NVS open failed");
        return;
    }
    p.putUInt  (kNvsKeyPrevPart,  running->address);
    p.putUInt  (kNvsKeyPrevBuild, prevBuild);
    p.putString(kNvsKeyPrevHash,  prevHash);
    p.putString(kNvsKeyPrevVer,   prevVer);
    p.end();
    SOTA_LOG("snapshot: prev_part=0x%x prev_build=%u",
             (unsigned)running->address, (unsigned)prevBuild);
}

// One-shot boot-time check. Runs at most once per process lifetime (gated by
// _bootValidated). Based on sota_trial value:
//   0 / missing -> nothing to do (steady state)
//   1           -> a trial is in flight; compare running partition to the
//                  snapshot. If we're on the new image, arm the confirm
//                  timer and set _inTrial. If we're somehow on the old
//                  image (e.g., bootloader skipped) treat as already-rolled-
//                  back and queue the report.
//   2           -> a rollback was performed on a prior boot; queue the
//                  rolled_back report for the next successful /check/.
void SimpleOTAClient::processBootValidation() {
    if (_bootValidated) return;
    _bootValidated = true;

    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) {
        SOTA_LOG("bootval: NVS open failed");
        return;
    }
    uint8_t  trial       = p.getUChar (kNvsKeyTrial,       0);
    uint32_t prevPart    = p.getUInt  (kNvsKeyPrevPart,    0);
    uint8_t  confirmPend = p.getUChar (kNvsKeyConfirmPend, 0);
    String   pendDep     = p.getString(kNvsKeyFailDep,     "");
    uint32_t pendBuild   = p.getUInt  (kNvsKeyFailBuild,   0);
    p.end();

    // No trial in flight: the only thing that may be pending is a
    // "confirmed" report from a successful apply() on a device with
    // rollback disabled. Surface it for the next /check/ to report.
    if (trial == 0) {
        if (confirmPend && !pendDep.isEmpty() && pendBuild) {
            _confirmedDep     = pendDep;
            _confirmedBuild   = pendBuild;
            _confirmedPending = true;
            SOTA_LOG("bootval: confirmed report pending (deployment=%s build=%u)",
                     pendDep.c_str(), (unsigned)pendBuild);
        }
        return;
    }

    // Rollback disabled: still process any trial state left in NVS from a
    // prior boot when rollback was enabled, so stale state does not persist.
    // trial=2: the old boot rolled back; still surface the report.
    // trial=1: a trial was armed but we no longer roll back. Auto-confirm:
    //          surface a "confirmed" report (the new image is running) and
    //          wipe trial state.
    if (!_rollbackEnabled) {
        if (trial == 2) {
            _rolledBackPending = true;
            SOTA_LOG("bootval: rollback disabled; pending rolled_back report");
        } else {
            if (confirmPend && !pendDep.isEmpty() && pendBuild) {
                _confirmedDep     = pendDep;
                _confirmedBuild   = pendBuild;
                _confirmedPending = true;
                SOTA_LOG("bootval: rollback disabled; auto-confirming residual trial");
            } else {
                SOTA_LOG("bootval: rollback disabled; clearing residual trial state");
            }
            clearTrialState();
        }
        return;
    }

    if (trial == 2) {
        // Rollback already happened; the report is pending until check()
        // calls reportRolledBackIfPending(). Any confirm-pending flag set
        // by the apply() that armed this trial is for the FAILED build:
        // it is dropped by reportRolledBackIfPending() once the server accepts.
        _rolledBackPending = true;
        SOTA_LOG("bootval: rollback pending report");
        return;
    }

    // trial == 1: this boot is the TRIAL boot of a new image. Verify we are
    // running on a different partition than the snapshot.
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        SOTA_LOG("bootval: no running partition; clearing trial state");
        clearTrialState();
        {   // Unrecoverable state: also wipe the confirm/fail context.
            Preferences pf;
            if (pf.begin(kNvsNamespace, /*readOnly=*/false)) {
                pf.remove(kNvsKeyFailDep);
                pf.remove(kNvsKeyFailBuild);
                pf.remove(kNvsKeyConfirmPend);
                pf.end();
            }
        }
        return;
    }

    if (prevPart == 0) {
        // Snapshot was incomplete (e.g., power loss between writes). We
        // can't safely rollback without a target; clear the flag and warn.
        SOTA_LOG("bootval: trial set but prev_part missing; clearing");
        clearTrialState();
        {   // Cannot verify partition; wipe the confirm/fail context too.
            Preferences pf;
            if (pf.begin(kNvsNamespace, /*readOnly=*/false)) {
                pf.remove(kNvsKeyFailDep);
                pf.remove(kNvsKeyFailBuild);
                pf.remove(kNvsKeyConfirmPend);
                pf.end();
            }
        }
        return;
    }

    if (running->address == prevPart) {
        // Bootloader did not switch to the new partition (e.g., new image
        // failed signature check). Treat as already-rolled-back.
        SOTA_LOG("bootval: still on prev partition; reporting rollback");
        // Restore the prev_* metadata into the live NVS keys so the device
        // reports the build it's actually running.
        Preferences rp;
        if (rp.begin(kNvsNamespace, /*readOnly=*/false)) {
            uint32_t pb = rp.getUInt  (kNvsKeyPrevBuild, 0);
            String   ph = rp.getString(kNvsKeyPrevHash, "");
            String   pv = rp.getString(kNvsKeyPrevVer,  "");
            if (pb)            rp.putUInt  (kNvsKeyBuild,   pb);
            if (ph.length())   rp.putString(kNvsKeyHash,    ph);
            if (pv.length())   rp.putString(kNvsKeyVersion, pv);
            rp.putUChar(kNvsKeyTrial, 2);
            rp.end();
        }
        _rolledBackPending = true;
        return;
    }

    // We're on the new image. Arm the trial.
    _inTrial = true;
    _confirmTimer = xTimerCreate("sota_confirm",
                                 pdMS_TO_TICKS(_confirmTimeoutSec * 1000UL),
                                 pdFALSE,   // one-shot
                                 this,      // timer ID = instance pointer
                                 &SimpleOTAClient::_confirmTimerCb);
    if (_confirmTimer == nullptr) {
        SOTA_LOG("bootval: timer alloc failed; rolling back immediately");
        performRollback();
        return;
    }
    if (xTimerStart(_confirmTimer, 0) != pdPASS) {
        SOTA_LOG("bootval: timer start failed; rolling back immediately");
        xTimerDelete(_confirmTimer, 0);
        _confirmTimer = nullptr;
        performRollback();
        return;
    }
    SOTA_LOG("bootval: TRIAL armed (timeout=%us)", (unsigned)_confirmTimeoutSec);
}

// FreeRTOS one-shot timer callback. Runs in the timer service task context.
// Safe to call performRollback() here: it does NVS writes + esp_restart().
void SimpleOTAClient::_confirmTimerCb(TimerHandle_t xTimer) {
    SimpleOTAClient* self =
        static_cast<SimpleOTAClient*>(pvTimerGetTimerID(xTimer));
    // Guard against the race where confirmRunning() ran concurrently with
    // this callback being queued. xTimerStop() can return pdPASS while the
    // callback is already in the timer service queue; reading _inTrial here
    // (which confirmRunning() sets false) eliminates the spurious-rollback
    // window without needing a mutex (bool write/read is atomic on Cortex-M).
    if (!self || !self->_inTrial) return;
    SOTA_LOG("confirm timer fired; rolling back");
    self->performRollback();
}

// Perform the rollback: set boot partition back to the snapshot, restore the
// pre-OTA NVS keys, mark trial=2 so the next boot reports it, then reboot.
// On any failure we still reboot; being stuck in a forever-trial state with
// a broken image would be worse than a partial restore.
void SimpleOTAClient::performRollback() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) {
        SOTA_LOG("rollback: NVS open failed; rebooting anyway");
        esp_restart();
        return;
    }
    uint32_t prevPart  = p.getUInt  (kNvsKeyPrevPart, 0);
    uint32_t prevBuild = p.getUInt  (kNvsKeyPrevBuild, 0);
    String   prevHash  = p.getString(kNvsKeyPrevHash, "");
    String   prevVer   = p.getString(kNvsKeyPrevVer,  "");

    if (prevPart != 0) {
        // Walk the OTA partitions and pick the one matching prevPart.
        const esp_partition_t* match = nullptr;
        for (esp_partition_subtype_t st = ESP_PARTITION_SUBTYPE_APP_OTA_0;
             st <= ESP_PARTITION_SUBTYPE_APP_OTA_15;
             st = (esp_partition_subtype_t)(st + 1)) {
            const esp_partition_t* part =
                esp_partition_find_first(ESP_PARTITION_TYPE_APP, st, nullptr);
            if (part && part->address == prevPart) { match = part; break; }
        }
        if (!match) {
            // Also check the factory partition as a fallback.
            const esp_partition_t* fac =
                esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                         ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                         nullptr);
            if (fac && fac->address == prevPart) match = fac;
        }

        if (match) {
            esp_err_t err = esp_ota_set_boot_partition(match);
            if (err != ESP_OK) {
                SOTA_LOG("rollback: set_boot_partition err=0x%x", (unsigned)err);
            } else {
                SOTA_LOG("rollback: boot partition set to 0x%x",
                         (unsigned)match->address);
            }
        } else {
            SOTA_LOG("rollback: no partition matches prev_part=0x%x",
                     (unsigned)prevPart);
        }
    }

    // Restore the live NVS keys so the rolled-back image reports its real
    // identity on next boot. Only restore non-empty/non-zero values.
    if (prevBuild)         p.putUInt  (kNvsKeyBuild,   prevBuild);
    if (prevHash.length()) p.putString(kNvsKeyHash,    prevHash);
    if (prevVer.length())  p.putString(kNvsKeyVersion, prevVer);

    // Mark trial=2 so the next boot's processBootValidation() queues the
    // rolled_back report. fail_dep / fail_build are retained for the report.
    p.putUChar(kNvsKeyTrial, 2);
    p.end();

    SOTA_LOG("rollback: rebooting");
    // NVS data is persisted by p.end() above. Reboot immediately; delay()
    // must not be called here as this function may run in the FreeRTOS timer
    // service task where vTaskDelay is forbidden.
    esp_restart();
}

// Wipe the trial snapshot NVS keys (trial flag + prev_* image identity).
// Does NOT touch the fail_* / conf_pend keys: those carry the confirm/rollback
// POST context and must survive until the server accepts the report so the
// event can be retried across reboots. reportConfirmedIfPending() and
// reportRolledBackIfPending() are responsible for removing those keys.
void SimpleOTAClient::clearTrialState() {
    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
    p.remove(kNvsKeyTrial);
    p.remove(kNvsKeyPrevPart);
    p.remove(kNvsKeyPrevBuild);
    p.remove(kNvsKeyPrevHash);
    p.remove(kNvsKeyPrevVer);
    p.end();
}

bool SimpleOTAClient::confirmRunning() {
    // Ensure boot validation has run regardless of whether begin()/check()
    // has been called yet. This makes confirmRunning() safe to call from
    // setup() in polling mode, before the first check() invocation.
    processBootValidation();
    if (!_inTrial) return false;
    // Set _inTrial false BEFORE stopping the timer. The FreeRTOS timer
    // callback (_confirmTimerCb) guards on _inTrial; setting it false first
    // maximises the window in which a concurrently-queued callback sees the
    // cancellation and aborts instead of calling performRollback().
    _inTrial = false;
    if (_confirmTimer) {
        xTimerStop(_confirmTimer, 0);
        xTimerDelete(_confirmTimer, 0);
        _confirmTimer = nullptr;
    }
    // Capture the confirm context from NVS BEFORE clearTrialState() wipes
    // it, so reportConfirmedIfPending() can POST a "confirmed" event for
    // this deployment on the next successful /check/.
    Preferences p;
    if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
        _confirmedDep   = p.getString(kNvsKeyFailDep,   "");
        _confirmedBuild = p.getUInt  (kNvsKeyFailBuild, 0);
        p.end();
        if (!_confirmedDep.isEmpty() && _confirmedBuild != 0) {
            _confirmedPending = true;
        }
    }
    clearTrialState();
    SOTA_LOG("confirmRunning: trial confirmed");
    return true;
}

// Report a rolled_back event if one is pending. Called from check() on each
// successful round-trip. On accepted=true, clears fail_* + trial flag.
void SimpleOTAClient::reportRolledBackIfPending() {
    if (!_rolledBackPending) return;

    Preferences p;
    if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return;
    String   failDep   = p.getString(kNvsKeyFailDep, "");
    uint32_t failBuild = p.getUInt  (kNvsKeyFailBuild, 0);
    p.end();

    if (failDep.isEmpty() || failBuild == 0) {
        // Nothing usable to report; clear everything so we don't loop forever.
        SOTA_LOG("rolled_back: no failure context; clearing");
        clearTrialState();
        Preferences pf;
        if (pf.begin(kNvsNamespace, /*readOnly=*/false)) {
            pf.remove(kNvsKeyFailDep);
            pf.remove(kNvsKeyFailBuild);
            pf.remove(kNvsKeyConfirmPend);
            pf.end();
        }
        _rolledBackPending = false;
        return;
    }

    bool ok = sendStatusFor("rolled_back", "confirm_timeout",
                            failDep.c_str(), failBuild);
    if (ok) {
        SOTA_LOG("rolled_back: server acknowledged");
        clearTrialState();
        Preferences pf;
        if (pf.begin(kNvsNamespace, /*readOnly=*/false)) {
            pf.remove(kNvsKeyFailDep);
            pf.remove(kNvsKeyFailBuild);
            pf.remove(kNvsKeyConfirmPend);
            pf.end();
        }
        _rolledBackPending = false;
    } else {
        SOTA_LOG("rolled_back: server did not accept; will retry");
    }
}

// Report a confirmed event if one is pending. Called from check() on each
// successful round-trip. On accepted=true, clears _confirmedPending and the
// underlying NVS keys (sota_conf_pend + sota_fail_*). The trial-path callers
// (confirmRunning) already wipe NVS via clearTrialState() before this fires;
// the rollback-disabled path relies on the NVS removals below.
void SimpleOTAClient::reportConfirmedIfPending() {
    if (!_confirmedPending) return;
    if (_confirmedDep.isEmpty() || _confirmedBuild == 0) {
        SOTA_LOG("confirmed: no deployment context; clearing");
        _confirmedPending = false;
        return;
    }
    bool ok = sendStatusFor("confirmed", nullptr,
                            _confirmedDep.c_str(), _confirmedBuild);
    if (ok) {
        SOTA_LOG("confirmed: server acknowledged");
        _confirmedPending = false;
        _confirmedDep     = "";
        _confirmedBuild   = 0;
        Preferences p;
        if (p.begin(kNvsNamespace, /*readOnly=*/false)) {
            p.remove(kNvsKeyConfirmPend);
            p.remove(kNvsKeyFailDep);
            p.remove(kNvsKeyFailBuild);
            p.end();
        }
    } else {
        SOTA_LOG("confirmed: server did not accept; will retry");
    }
}
