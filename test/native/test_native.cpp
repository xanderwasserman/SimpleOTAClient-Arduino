/**
 * Host-side unit tests for signing and rollback decisions (no Arduino).
 *
 * Build & run (from the repository root; mirrors the CI native-test job):
 *
 *   cc  -std=c99   -Wall -Wextra -Isrc -c src/monocypher.c src/monocypher-ed25519.c
 *   g++ -std=c++11 -Wall -Wextra -Isrc -o test_native \
 *       test/native/test_native.cpp src/SimpleOTASigning.cpp \
 *       monocypher.o monocypher-ed25519.o && ./test_native
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

    // --- hold the image only for a real SimpleOTA trial (M1) ---------------
    CHECK(sotaHoldPendingImage(true, true, 1, 0x20000, false),
          "hold: pending trial on the new partition");
    CHECK(!sotaHoldPendingImage(true, true, 0, 0, false),
          "release: pending image with no trial record (ArduinoOTA / HTTPUpdate)");
    CHECK(!sotaHoldPendingImage(true, false, 1, 0x20000, false),
          "release: trial byte unreadable");
    CHECK(!sotaHoldPendingImage(true, true, 1, 0, false),
          "release: trial with no previous partition");
    CHECK(!sotaHoldPendingImage(false, true, 1, 0x20000, false),
          "release: image is already valid");
    CHECK(!sotaHoldPendingImage(true, true, 1, 0x20000, true),
          "release: already running the previous partition");
    CHECK(sotaShouldMarkImageValid(true, true, 0, true, 0, false),
          "mark valid: pending image that is not ours");
    CHECK(!sotaShouldMarkImageValid(true, true, 1, true, 0x20000, false),
          "do not mark valid: held SimpleOTA trial");
    CHECK(sotaShouldMarkImageValid(true, true, 1, false, 0x20000, false),
          "mark valid: rollback turned off");

    // --- power loss after confirm, and the NVS-only timer (M2) -------------
    CHECK(sotaTrialBootAction(true, false, true, 1, true, 0x20000, false, true)
              == SOTA_TRIAL_CONFIRMED,
          "already valid + trial record is confirmed, not rolled back");
    CHECK(sotaTrialBootAction(true, true, true, 1, true, 0x20000, false, true)
              == SOTA_TRIAL_HOLD,
          "still unconfirmed trial arms the timer");
    CHECK(sotaTrialBootAction(false, false, true, 1, true, 0x20000, false, true)
              == SOTA_TRIAL_HOLD,
          "without bootloader rollback the NVS timer still arms");
    CHECK(sotaTrialBootAction(true, false, true, 1, true, 0x20000, true, true)
              == SOTA_TRIAL_ROLLED_BACK,
          "running the previous partition is a rollback");
    CHECK(sotaTrialBootAction(true, true, true, 1, false, 0x20000, false, true)
              == SOTA_TRIAL_ACCEPT_RESIDUAL,
          "rollback disabled accepts a residual trial");

    // --- one phase so confirm, rollback, and timeout cannot race (S3/S4) ---
    uint8_t next = 0;
    CHECK(sotaClaimPhase(SOTA_PHASE_TRIAL, SOTA_CLAIM_CONFIRM, &next)
              && next == SOTA_PHASE_CONFIRMING,
          "confirm claims TRIAL");
    CHECK(sotaClaimPhase(SOTA_PHASE_TRIAL, SOTA_CLAIM_ROLLBACK, &next)
              && next == SOTA_PHASE_ROLLING_BACK,
          "rollback claims TRIAL");
    CHECK(!sotaClaimPhase(SOTA_PHASE_CONFIRMING, SOTA_CLAIM_ROLLBACK, &next),
          "rollback loses once confirm has claimed");
    CHECK(!sotaTimeoutRearmAllowed(SOTA_PHASE_CONFIRMING),
          "setConfirmTimeout does not re-arm after confirm");
    CHECK(!sotaTimeoutRearmAllowed(SOTA_PHASE_IDLE),
          "setConfirmTimeout does not re-arm when idle");
    CHECK(sotaTimeoutRearmAllowed(SOTA_PHASE_TRIAL),
          "setConfirmTimeout re-arms during a trial");

    // --- timer period does not wrap past 4294 s (S1) -----------------------
    CHECK(sotaConfirmTimerTicks(7200, 1000) == 7200000u,
          "7200 s at 1000 Hz is 7200000 ticks");
    CHECK(sotaConfirmTimerTicks(86400, 1000) == 86400000u,
          "86400 s at 1000 Hz is 86400000 ticks");

    // --- proportional margin and calibrated slow clock (S5) ----------------
    CHECK(sotaWatchMarginSec(300, 60) == 60u, "300 s margin stays at 60 s");
    CHECK(sotaWatchMarginSec(7200, 60) == 720u, "7200 s margin is 10 percent");
    CHECK(sotaWatchMarginSec(100, 60) == 60u, "short timeout keeps the 60 s floor");
    const uint64_t us = 360ull * 1000000ull;
    const uint32_t cal = (uint32_t)(((uint64_t)1000000 << 19) / 150000u);
    CHECK(sotaSlowclkTicks(us, cal) > 0, "calibrated ticks are non-zero");
    CHECK(sotaSlowclkTicks(us, 0) == 0, "a zero calibration asks for the nominal fallback");

    // --- rollback reason (S7) ----------------------------------------------
    CHECK(strcmp(sotaRollbackReason(SOTA_RST_PANIC, "confirm_timeout"),
                 "confirm_timeout") == 0,
          "stored confirm_timeout wins over the software reset");
    CHECK(strcmp(sotaRollbackReason(SOTA_RST_PANIC, ""), "panic") == 0,
          "panic reset with no stored reason");
    CHECK(strcmp(sotaRollbackReason(SOTA_RST_WDT, nullptr), "watchdog") == 0,
          "chip watchdog reset");
    CHECK(strcmp(sotaRollbackReason(SOTA_RST_SW, nullptr), "reset") == 0,
          "software reset with no stored reason");
    CHECK(strcmp(sotaReportRollbackReason("panic"), "panic") == 0,
          "the report sends the reason saved when the rollback was detected");
    CHECK(strcmp(sotaReportRollbackReason(""), "reset") == 0,
          "a missing saved reason is not taken from a later boot");

    printf("===========================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
