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
      _insecureWarned(false),
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
const char* SimpleOTAClient::lastOfferedVersion() const       { return _offeredVersion.c_str(); }

void SimpleOTAClient::begin(uint32_t checkIntervalSec,
                      void (*onResult)(OTAResult),
                      bool (*isConnected)()) {
    if (_taskHandle != nullptr) return;
    _checkIntervalSec = checkIntervalSec;
    _onResult = onResult;
    _isConnected = isConnected;
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
// Never retry on any received HTTP response, including non-2xx.
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
                return code;
            }
            SOTA_LOG("POST %s transport error %d (attempt %d)", path, code, attempt);
            http.end();
        }
        if (attempt == 0) delay(2000);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// /api/v1/ota/check/
// ---------------------------------------------------------------------------

bool SimpleOTAClient::check() {
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
    int code = postJson("/api/v1/ota/check/", body, &resp);
    if (code != 200) {
        SOTA_LOG("check: non-200 (%d), no offer", code);
        return false;
    }

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

bool SimpleOTAClient::sendStatus(const char* event, const char* reason) {
    String body;
    body.reserve(192);
    body += '{';
    body += "\"device_id\":";     appendJsonString(body, _deviceId);
    body += ",\"deployment_id\":"; appendJsonString(body, _deploymentId.c_str());
    body += ",\"event\":";         appendJsonString(body, event);
    body += ",\"build_number\":";
    body += String(_buildNumber);
    if (reason) {
        body += ",\"reason\":";    appendJsonString(body, reason);
    }
    body += '}';

    String resp;
    int code = postJson("/api/v1/ota/status/", body, &resp);
    if (code < 200 || code >= 300) return false;
    bool accepted = false;
    jsonGetBool(resp, "accepted", accepted);
    return accepted;
}

bool SimpleOTAClient::report(const char* event, const char* reason) {
    // sendStatus() includes deployment_id and build_number; without a
    // deployment ID the backend can't correlate the event. _deploymentId is
    // populated by check() and retained through a successful apply() until
    // the next check() invocation, so report() is valid both while an offer
    // is pending and in the post-apply window (e.g. report("rebooted")).
    if (_deploymentId.isEmpty()) {
        SOTA_LOG("report: ignored (no deployment context)");
        return false;
    }
    return sendStatus(event, reason);
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
    writeBuildNumber(_buildNumber);
    writeHashToNvs(hex);
    writeVersionToNvs(_offeredVersion.c_str());
    sendStatus("validated", nullptr);

    // Clear the download-related fields so a second apply() call (with
    // auto-reboot disabled) doesn't redownload and reflash the same image.
    // _deploymentId and _buildNumber are deliberately retained so the
    // application can call report() (e.g. report("rebooted")) for events that
    // are temporally adjacent to this apply(). The next check() invocation
    // calls clearOffer() at its top, which resets that context.
    _hasOffer       = false;
    _url            = "";
    _checksum       = "";
    _size           = 0;
    _offeredVersion = "";

    if (_autoReboot) {
        SOTA_LOG("apply: rebooting");
        delay(100);
        esp_restart();
        // not reached
    }
    return OTA_SUCCESS;
}
