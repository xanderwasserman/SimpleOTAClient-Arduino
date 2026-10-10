/**
 * SimpleOTASigning.h - Ed25519 firmware-signature helpers for SimpleOTAClient.
 *
 * Portable (no Arduino dependencies) so it can be unit-tested on a host
 * machine with plain g++; see test/native/test_native.cpp.
 *
 * The SimpleOTA server delivers, for artifacts uploaded with
 * security_mode "signed", a base64 Ed25519 signature computed over the raw
 * firmware bytes exactly as downloaded. The device pins the project's
 * PUBLIC key (it is not a secret) and must verify the signature over the
 * downloaded bytes BEFORE the new image is marked bootable.
 *
 * Verification is incremental (Monocypher's streaming Ed25519 check) so the
 * image never has to fit in RAM: apply() feeds the same 1 KB chunks it
 * writes to the OTA partition.
 */

#ifndef SIMPLEOTA_SIGNING_H
#define SIMPLEOTA_SIGNING_H

#include <stddef.h>
#include <stdint.h>

#include "monocypher-ed25519.h"

/**
 * @brief Parse a PEM "BEGIN PUBLIC KEY" (SubjectPublicKeyInfo) Ed25519 key.
 *
 * Accepts the exact format the SimpleOTA dashboard and API hand out. The
 * DER inside an Ed25519 SPKI is fixed-size (44 bytes: a constant 12-byte
 * header followed by the 32 raw key bytes), so no ASN.1 parser is needed.
 *
 * @param pem  Null-terminated PEM text (armor lines + base64 body).
 * @param out  Receives the raw 32-byte public key on success.
 * @return true on success; false for malformed PEM, wrong key type, or
 *         wrong length. `out` is untouched on failure.
 */
/**
 * @brief Outcome of the signed-firmware policy decision (see sotaSignedGate).
 */
enum SotaSignedGate {
    SOTA_GATE_SKIP,             ///< Not a signed update; apply normally.
    SOTA_GATE_VERIFY,           ///< Verify the signature during download.
    SOTA_GATE_FAIL_CLOSED,      ///< A signature is required but absent/unusable; reject now.
    SOTA_GATE_WARN_UNVERIFIED,  ///< Signed offer but no key to check it; apply + warn.
};

/**
 * @brief Decide how to handle a (possibly signed) offer, from device policy
 *        and the offer's own claims. Pure and host-testable.
 *
 * The anti-downgrade guarantee lives here: a device that declares signed
 * mode AND has a key pinned MUST see a usable signature, no matter what the
 * offer claims. A compromised server or MITM that strips the `security_mode`
 * / `signature` fields from the response therefore cannot silently downgrade
 * the device to checksum-only flashing; it gets SOTA_GATE_FAIL_CLOSED. When
 * the device is not enforcing (not in signed mode, or no key pinned), the
 * offer's own declaration is trusted, which keeps basic-to-signed fleet
 * migration flowing.
 *
 * @param deviceReportsSigned  Device is configured with security_mode "signed".
 * @param numKeys              Count of pinned public keys (0..2).
 * @param offerSigned          The offer's security_mode is "signed".
 * @param offerHasSignature    A usable (decoded, ed25519) signature is present.
 */
SotaSignedGate sotaSignedGate(bool deviceReportsSigned, uint8_t numKeys,
                              bool offerSigned, bool offerHasSignature);

bool sotaParseEd25519PublicKeyPem(const char* pem, uint8_t out[32]);

/**
 * @brief Decode a base64 Ed25519 signature (as sent in the OTA check
 *        response) into its raw 64 bytes.
 * @return true only if the input is valid base64 of exactly 64 bytes.
 */
bool sotaDecodeSignatureB64(const char* b64, uint8_t out[64]);

/**
 * @brief Streaming Ed25519 verifier over one firmware image, for up to two
 *        candidate public keys (key rotation windows).
 *
 * Runs one Monocypher incremental check context per candidate key; the
 * signature is accepted if it verifies under ANY candidate. Contexts are a
 * few hundred bytes each (embedded SHA-512 state), so two in parallel are
 * cheap next to the 1 KB download buffer.
 *
 * Usage: begin() once, update() per downloaded chunk, final() once.
 */
class SotaSignatureVerifier {
public:
    static const uint8_t kMaxKeys = 2;

    /**
     * @param sig      Raw 64-byte signature from the OTA offer.
     * @param keys     Array of raw 32-byte public keys.
     * @param numKeys  1..kMaxKeys candidate keys (extra keys are ignored).
     * @return false (verifier inert; final() will fail) when numKeys is 0.
     */
    bool begin(const uint8_t sig[64], const uint8_t keys[][32], uint8_t numKeys);

    /** Feed the next chunk of the image, in download order. */
    void update(const uint8_t* data, size_t len);

    /** @return true if the signature verifies under any candidate key. */
    bool final();

private:
    crypto_check_ed25519_ctx _ctx[kMaxKeys];
    uint8_t _numKeys = 0;
};

#endif  // SIMPLEOTA_SIGNING_H
