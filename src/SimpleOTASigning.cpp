/**
 * SimpleOTASigning.cpp - implementation. See header for the contract.
 *
 * Kept free of Arduino headers on purpose: everything here compiles with
 * plain g++ for the host-side unit tests in test/native/.
 */

#include "SimpleOTASigning.h"

#include <string.h>

// ---------------------------------------------------------------------------
// base64 (decode only)
// ---------------------------------------------------------------------------

// Returns the 0..63 value of a base64 digit, or -1 for anything else
// (including '=' padding, which the caller handles explicitly).
static int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decode base64 from `in` (whitespace/newlines skipped, '=' padding
// tolerated at the end) into `out`, which holds `outCap` bytes. Returns the
// number of bytes written, or -1 on any malformed input or overflow.
static int b64Decode(const char* in, uint8_t* out, size_t outCap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t written = 0;
    bool sawPad = false;

    for (const char* p = in; *p; ++p) {
        char c = *p;
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') { sawPad = true; continue; }
        if (sawPad) return -1;  // data after padding
        int v = b64Value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= outCap) return -1;
            out[written++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    // Leftover bits must be zero padding only (canonical-ish check).
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) return -1;
    return (int)written;
}

// ---------------------------------------------------------------------------
// Signed-firmware policy
// ---------------------------------------------------------------------------

SotaSignedGate sotaSignedGate(bool deviceReportsSigned, uint8_t numKeys,
                              bool offerSigned, bool offerHasSignature) {
    // Enforcing: the device declares signed mode and can actually verify.
    // A signature is then mandatory regardless of what the offer claims,
    // so a stripped-signature response fails closed rather than downgrading.
    if (deviceReportsSigned && numKeys > 0) {
        return offerHasSignature ? SOTA_GATE_VERIFY : SOTA_GATE_FAIL_CLOSED;
    }
    // Not enforcing: trust the offer's own declaration.
    if (!offerSigned) {
        return SOTA_GATE_SKIP;
    }
    if (numKeys == 0) {
        // Signed offer but nothing to check it against (e.g. a device
        // mid-migration that has not pinned a key yet). Best effort.
        return SOTA_GATE_WARN_UNVERIFIED;
    }
    // Keys pinned and the offer claims signed: a usable signature is
    // expected; its absence is treated as a rejectable anomaly.
    return offerHasSignature ? SOTA_GATE_VERIFY : SOTA_GATE_FAIL_CLOSED;
}

// ---------------------------------------------------------------------------
// PEM parsing
// ---------------------------------------------------------------------------

// DER header of an Ed25519 SubjectPublicKeyInfo (RFC 8410):
//   SEQUENCE(42) { SEQUENCE(5) { OID 1.3.101.112 }, BIT STRING(33) 0x00 ... }
// The full structure is always exactly 44 bytes: these 12, then the raw key.
static const uint8_t kEd25519SpkiHeader[12] = {
    0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00,
};

bool sotaParseEd25519PublicKeyPem(const char* pem, uint8_t out[32]) {
    if (!pem) return false;

    const char* begin = strstr(pem, "-----BEGIN PUBLIC KEY-----");
    if (!begin) return false;
    begin += strlen("-----BEGIN PUBLIC KEY-----");
    const char* end = strstr(begin, "-----END PUBLIC KEY-----");
    if (!end) return false;

    // Copy the base64 body into a bounded scratch buffer. An Ed25519 SPKI
    // body is 60 base64 chars; allow generous slack for line breaks.
    char body[128];
    size_t n = (size_t)(end - begin);
    if (n >= sizeof(body)) return false;
    memcpy(body, begin, n);
    body[n] = '\0';

    uint8_t der[64];
    int derLen = b64Decode(body, der, sizeof(der));
    if (derLen != 44) return false;
    if (memcmp(der, kEd25519SpkiHeader, sizeof(kEd25519SpkiHeader)) != 0) {
        return false;  // valid PEM but not an Ed25519 public key
    }
    memcpy(out, der + sizeof(kEd25519SpkiHeader), 32);
    return true;
}

bool sotaDecodeSignatureB64(const char* b64, uint8_t out[64]) {
    if (!b64) return false;
    uint8_t buf[80];
    int n = b64Decode(b64, buf, sizeof(buf));
    if (n != 64) return false;
    memcpy(out, buf, 64);
    return true;
}

// ---------------------------------------------------------------------------
// SotaSignatureVerifier
// ---------------------------------------------------------------------------

bool SotaSignatureVerifier::begin(const uint8_t sig[64], const uint8_t keys[][32],
                                  uint8_t numKeys) {
    _numKeys = (numKeys > kMaxKeys) ? kMaxKeys : numKeys;
    for (uint8_t i = 0; i < _numKeys; ++i) {
        crypto_ed25519_check_init((crypto_check_ctx_abstract*)&_ctx[i], sig,
                                  keys[i]);
    }
    return _numKeys > 0;
}

void SotaSignatureVerifier::update(const uint8_t* data, size_t len) {
    for (uint8_t i = 0; i < _numKeys; ++i) {
        crypto_ed25519_check_update((crypto_check_ctx_abstract*)&_ctx[i], data,
                                    len);
    }
}

bool SotaSignatureVerifier::final() {
    // Run EVERY context to completion (no early return) so timing does not
    // depend on which candidate key matched.
    bool ok = false;
    for (uint8_t i = 0; i < _numKeys; ++i) {
        if (crypto_ed25519_check_final((crypto_check_ctx_abstract*)&_ctx[i]) == 0) {
            ok = true;
        }
    }
    _numKeys = 0;  // contexts are consumed; require a fresh begin()
    return ok;
}
