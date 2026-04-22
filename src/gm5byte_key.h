/* gm5byte_key.h — GM 5-byte seed-key derivation for T87A (algo 135)
 *
 * Uses precomputed AES keys (SHA-256 chain done offline) + AES-128 encrypt.
 * Only 11 possible AES keys exist for algo 135 (min_seed=245, seed[4]=0..10).
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

#ifdef __cplusplus
}
#endif

#endif /* GM5BYTE_KEY_H */
