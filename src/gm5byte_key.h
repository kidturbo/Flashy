/* gm5byte_key.h — GM 5-byte seed-key derivation
 *
 * Uses precomputed AES keys (SHA-256 chain done offline) + AES-128 encrypt.
 * Only 11 possible AES keys per algo (min_seed=245, seed[4]=0..10).
 *
 * Supports two algos (same cipher core, different precomputed tables):
 *   - Algo 135: T87A 8L90 TCM (gm5byte_compute_key)
 *   - Algo 146: E92A late-model E92 ECM (2017+) (gm5byte_compute_key_e92a)
 *     Bench-verified 2026-04-25 against captured seed=8785EEC106
 *     -> key=08B3B3656D, ECU returned $67 02.
 */
#ifndef GM5BYTE_KEY_H
#define GM5BYTE_KEY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute 5-byte key from 5-byte seed using T87A algo 135.
 * Returns true on success, false if seed[4] > 10 (out of range).
 * key_out must point to a buffer of at least 5 bytes. */
bool gm5byte_compute_key(const uint8_t seed[5], uint8_t key_out[5]);

/* Compute 5-byte key from 5-byte seed using E92A algo 146.
 * Same return convention as gm5byte_compute_key. */
bool gm5byte_compute_key_e92a(const uint8_t seed[5], uint8_t key_out[5]);

#ifdef __cplusplus
}
#endif

#endif /* GM5BYTE_KEY_H */
