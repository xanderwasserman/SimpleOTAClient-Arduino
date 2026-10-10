/**
 * Host-side unit tests for SimpleOTASigning (no Arduino, no framework).
 *
 * Build & run (from the repository root; mirrors the CI native-test job):
 *
 *   cc  -std=c99   -Wall -Wextra -Isrc -c src/monocypher.c src/monocypher-ed25519.c
 *   g++ -std=c++11 -Wall -Wextra -Isrc -o test_signing \
 *       test/native/test_signing.cpp src/SimpleOTASigning.cpp \
 *       monocypher.o monocypher-ed25519.o && ./test_signing
 *
 * Covers:
 *  - RFC 8032 pure-Ed25519 test vectors through the streaming interface.
 *  - A vector generated with the actual SimpleOTA server stack (Python
 *    `cryptography`): PEM public key + base64 signature over a 3037-byte
 *    message, proving the server-signed artifact verifies on this C path.
 *  - Negative cases: tampered message, wrong key, truncated/garbage
 *    signature, malformed PEM, non-Ed25519 DER.
 *  - Chunk-boundary invariance: 1-byte streaming == one-shot.
 *  - Two-key rotation acceptance (correct key in either slot).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SimpleOTASigning.h"
#include "SimpleOTARollback.h"

static int g_failures = 0;

#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) {                                              \
            printf("PASS  %s\n", name);                          \
        } else {                                                 \
            printf("FAIL  %s (line %d)\n", name, __LINE__);      \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    if (strlen(hex) != outLen * 2) return false;
    for (size_t i = 0; i < outLen; ++i) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// Verify `msg` against `sig` with a single key, feeding `chunk`-sized pieces.
static bool verifyChunked(const uint8_t sig[64], const uint8_t key[32],
                          const uint8_t* msg, size_t len, size_t chunk) {
    uint8_t keys[1][32];
    memcpy(keys[0], key, 32);
    SotaSignatureVerifier v;
    v.begin(sig, keys, 1);
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        v.update(msg + off, n);
    }
    return v.final();
}

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

// RFC 8032 section 7.1, TEST 2: 1-byte message 0x72.
static const char* kRfc8032Pub2 =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const char* kRfc8032Sig2 =
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
    "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
static const uint8_t kRfc8032Msg2[1] = {0x72};

// RFC 8032 section 7.1, TEST 3: 2-byte message af82.
static const char* kRfc8032Pub3 =
    "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025";
static const char* kRfc8032Sig3 =
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
    "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a";
static const uint8_t kRfc8032Msg3[2] = {0xaf, 0x82};

// Vector generated with the SimpleOTA server stack (Python `cryptography`,
// the exact library that signs verification material in production). The
// message is 3037 bytes of (i*7+13)&0xFF, deliberately not chunk-aligned.
static const char* kServerPubPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEAB2HJU+VQ7QbclCJ1QOZAYKeSbt78tVkU74pDwJVYh0Y=\n"
    "-----END PUBLIC KEY-----\n";
static const char* kServerSigB64 =
    "RnV5622C7mDIKzs73+bTbKKAaBNDgFrpFg9EcfB/nAbT+QzLRzznHavLSjgjeEMc"
    "0NmhNtKk9qUOESyG4FqmDg==";
static const char* kOtherPubPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEArGD0GjJUgwBdqQe1YXY0JIwlNdC/0g/ViSArBa9fXDg=\n"
    "-----END PUBLIC KEY-----\n";
static const size_t kServerMsgLen = 3037;

static void fillServerMsg(uint8_t* buf) {
    for (size_t i = 0; i < kServerMsgLen; ++i) {
        buf[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }
}

// An RSA-2048 SPKI PEM (valid PEM + DER, wrong algorithm): must be rejected.
static const char* kRsaPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0Z3VS5JJcds3xfn/ygWy\n"
    "F0mLxGZk3GG9tS+0RrGXi9K+PT47r1Vv7WeStkGxT2AwLYCSzMPOGDJbfBRWJVkI\n"
    "b3nDCONVn0DDF7VkxSTBEXTvJqqu69R2M8Xr32NnCzUleC0eq/eqhBpc8dSKUeqB\n"
    "8sJyxnywjm14sffJ4XXm3Or92BpTn6DkYA1KYIRLBLl3a3H9uKGoZbrPnozI7bMK\n"
    "aTgm27u2rBFsAy4pkxbDpgvzrSGRUJPTUAqxvjV/xnkTMQb8DTFXOLd06taWxNRJ\n"
    "eolbrTBTThLqbP2vAgMt2/oW1kKzXd1Y+HYX+GY1Kb2ARi/1zwqi/ArNErHVCLnK\n"
    "5QIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

// ---------------------------------------------------------------------------

int main(void) {
    printf("SimpleOTASigning host tests\n===========================\n");

    // --- PEM parsing -------------------------------------------------------
    uint8_t serverKey[32], otherKey[32], scratch[32];
    CHECK(sotaParseEd25519PublicKeyPem(kServerPubPem, serverKey),
          "parse server public key PEM");
    CHECK(sotaParseEd25519PublicKeyPem(kOtherPubPem, otherKey),
          "parse second public key PEM");
    CHECK(!sotaParseEd25519PublicKeyPem("not a pem at all", scratch),
          "reject garbage PEM");
    CHECK(!sotaParseEd25519PublicKeyPem(
              "-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n",
              scratch),
          "reject too-short DER");
    CHECK(!sotaParseEd25519PublicKeyPem(kRsaPem, scratch),
          "reject non-Ed25519 (RSA) SPKI");
    CHECK(!sotaParseEd25519PublicKeyPem(NULL, scratch), "reject NULL PEM");

    // --- signature decoding ------------------------------------------------
    uint8_t serverSig[64];
    CHECK(sotaDecodeSignatureB64(kServerSigB64, serverSig),
          "decode server signature base64");
    uint8_t tmp[64];
    CHECK(!sotaDecodeSignatureB64("QUJD", tmp), "reject 3-byte signature");
    CHECK(!sotaDecodeSignatureB64("@@!!", tmp), "reject invalid base64");
    CHECK(!sotaDecodeSignatureB64(NULL, tmp), "reject NULL signature");
    {
        // Truncated: drop the last 4 chars (3 bytes) of a valid signature.
        char trunc[96];
        strncpy(trunc, kServerSigB64, sizeof(trunc));
        trunc[sizeof(trunc) - 1] = 0;
        trunc[strlen(trunc) - 4] = 0;
        CHECK(!sotaDecodeSignatureB64(trunc, tmp), "reject truncated signature");
    }

    // --- RFC 8032 vectors via streaming -------------------------------------
    {
        uint8_t pub[32], sig[64];
        CHECK(hexToBytes(kRfc8032Pub2, pub, 32) && hexToBytes(kRfc8032Sig2, sig, 64),
              "load RFC 8032 test 2 vector");
        CHECK(verifyChunked(sig, pub, kRfc8032Msg2, 1, 1),
              "RFC 8032 test 2 verifies (streaming)");
        CHECK(hexToBytes(kRfc8032Pub3, pub, 32) && hexToBytes(kRfc8032Sig3, sig, 64),
              "load RFC 8032 test 3 vector");
        CHECK(verifyChunked(sig, pub, kRfc8032Msg3, 2, 1),
              "RFC 8032 test 3 verifies (1-byte chunks)");
        // Tamper: flip one bit.
        uint8_t bad[2] = {(uint8_t)(kRfc8032Msg3[0] ^ 0x01), kRfc8032Msg3[1]};
        CHECK(!verifyChunked(sig, pub, bad, 2, 2),
              "RFC 8032 test 3 rejects tampered message");
    }

    // --- server-stack vector -------------------------------------------------
    uint8_t* msg = (uint8_t*)malloc(kServerMsgLen);
    fillServerMsg(msg);

    CHECK(verifyChunked(serverSig, serverKey, msg, kServerMsgLen, 1024),
          "server-signed message verifies (1 KB chunks, apply()-style)");
    CHECK(verifyChunked(serverSig, serverKey, msg, kServerMsgLen, kServerMsgLen),
          "server-signed message verifies (one shot)");
    CHECK(verifyChunked(serverSig, serverKey, msg, kServerMsgLen, 1),
          "server-signed message verifies (1-byte chunks)");

    msg[1500] ^= 0x80;  // tamper mid-image
    CHECK(!verifyChunked(serverSig, serverKey, msg, kServerMsgLen, 1024),
          "tampered image rejected");
    msg[1500] ^= 0x80;  // restore

    CHECK(!verifyChunked(serverSig, otherKey, msg, kServerMsgLen, 1024),
          "wrong public key rejected");
    CHECK(!verifyChunked(serverSig, serverKey, msg, kServerMsgLen - 1, 1024),
          "short image rejected");

    // --- multi-key rotation ---------------------------------------------------
    {
        uint8_t keys[2][32];
        SotaSignatureVerifier v;

        // Correct key in slot 1 (rotation: old key first, new key second).
        memcpy(keys[0], otherKey, 32);
        memcpy(keys[1], serverKey, 32);
        v.begin(serverSig, keys, 2);
        v.update(msg, kServerMsgLen);
        CHECK(v.final(), "two pinned keys: matching key in slot 1 accepted");

        // Correct key in slot 0.
        memcpy(keys[0], serverKey, 32);
        memcpy(keys[1], otherKey, 32);
        v.begin(serverSig, keys, 2);
        v.update(msg, kServerMsgLen);
        CHECK(v.final(), "two pinned keys: matching key in slot 0 accepted");

        // Neither key matches.
        memcpy(keys[0], otherKey, 32);
        memcpy(keys[1], otherKey, 32);
        v.begin(serverSig, keys, 2);
        v.update(msg, kServerMsgLen);
        CHECK(!v.final(), "two wrong keys rejected");

        // Zero keys: inert verifier fails closed.
        CHECK(!v.begin(serverSig, keys, 0), "begin() with zero keys is inert");
        CHECK(!v.final(), "final() with zero keys fails");
    }

    free(msg);

    // --- signed-firmware policy gate ----------------------------------------
    // sotaSignedGate(deviceReportsSigned, numKeys, offerSigned, offerHasSignature)

    // Enforcing device (reports signed + key pinned): signature mandatory.
    CHECK(sotaSignedGate(true, 1, true, true) == SOTA_GATE_VERIFY,
          "gate: signed device + key + valid signed offer -> VERIFY");
    CHECK(sotaSignedGate(true, 1, true, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: signed device + key + signed offer, no usable sig -> FAIL_CLOSED");
    // The anti-downgrade case: server strips security_mode/signature.
    CHECK(sotaSignedGate(true, 1, false, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: signed device + key + UNSIGNED offer -> FAIL_CLOSED (no downgrade)");
    CHECK(sotaSignedGate(true, 2, false, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: signed device + 2 keys + unsigned offer -> FAIL_CLOSED");

    // Signed device but no key pinned yet (misconfig / mid-migration).
    CHECK(sotaSignedGate(true, 0, true, false) == SOTA_GATE_WARN_UNVERIFIED,
          "gate: signed device, no key, signed offer -> WARN_UNVERIFIED");
    CHECK(sotaSignedGate(true, 0, false, false) == SOTA_GATE_SKIP,
          "gate: signed device, no key, unsigned offer -> SKIP");

    // Non-enforcing device (not in signed mode): trust the offer.
    CHECK(sotaSignedGate(false, 0, false, false) == SOTA_GATE_SKIP,
          "gate: basic device, basic offer -> SKIP");
    CHECK(sotaSignedGate(false, 0, true, false) == SOTA_GATE_WARN_UNVERIFIED,
          "gate: basic device, no key, signed offer -> WARN_UNVERIFIED (migration)");
    CHECK(sotaSignedGate(false, 1, true, true) == SOTA_GATE_VERIFY,
          "gate: basic device, key pinned, valid signed offer -> VERIFY (defensive)");
    CHECK(sotaSignedGate(false, 1, true, false) == SOTA_GATE_FAIL_CLOSED,
          "gate: basic device, key pinned, signed offer w/o sig -> FAIL_CLOSED");
    CHECK(sotaSignedGate(false, 1, false, false) == SOTA_GATE_SKIP,
          "gate: basic device, key pinned, basic offer -> SKIP");

    // --- bootloader pending-image policy ------------------------------------
    // sotaPendingImageAction(pending, nvsOpen, trial, rollbackEnabled, prevPart, runningMatchesPrev)

    CHECK(sotaPendingImageAction(true, true, 1, true, 0x20000, false) == SOTA_IMAGE_HOLD,
          "pending: trial on the new partition stays pending");
    CHECK(sotaPendingImageAction(true, true, 0, true, 0, false) == SOTA_IMAGE_ACCEPT,
          "pending: no trial record is marked valid");
    CHECK(sotaPendingImageAction(true, false, 0, true, 0, false) == SOTA_IMAGE_ACCEPT,
          "pending: missing NVS namespace is marked valid");
    CHECK(sotaPendingImageAction(false, true, 1, true, 0x20000, false) == SOTA_IMAGE_LEAVE,
          "not pending: mark-valid is not called");
    CHECK(sotaPendingImageAction(true, true, 1, false, 0x20000, false) == SOTA_IMAGE_ACCEPT,
          "pending: rollback disabled accepts the image");
    CHECK(sotaPendingImageAction(true, true, 1, true, 0, false) == SOTA_IMAGE_ACCEPT,
          "pending: trial with no previous partition is accepted");
    CHECK(sotaPendingImageAction(true, true, 1, true, 0x20000, true) == SOTA_IMAGE_ACCEPT,
          "pending: already back on the previous partition is accepted");
    CHECK(sotaPendingImageAction(true, true, 2, true, 0x20000, false) == SOTA_IMAGE_ACCEPT,
          "pending: trial==2 is not held open");

    CHECK(sotaTrialWatchdogAction(true, true, 1) == SOTA_WDT_ARM,
          "watchdog: pending trial is armed");
    CHECK(sotaTrialWatchdogAction(true, true, 0) == SOTA_WDT_DISARM,
          "watchdog: pending without a trial record is disarmed");
    CHECK(sotaTrialWatchdogAction(true, false, 1) == SOTA_WDT_DISARM,
          "watchdog: unreadable NVS does not arm");
    CHECK(sotaTrialWatchdogAction(false, true, 1) == SOTA_WDT_DISARM,
          "watchdog: a confirmed image is not armed");

    CHECK(sotaApplyBlockedWhilePending(true),
          "apply: blocked while the running image is pending");
    CHECK(!sotaApplyBlockedWhilePending(false),
          "apply: allowed once the running image is not pending");

    printf("===========================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
