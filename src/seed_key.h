#ifndef SEED_KEY_H
#define SEED_KEY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GM seed-key bytecode interpreter.
 *
 * Each algorithm is a 13-byte bytecode sequence:
 *   [id, op1, arg1h, arg1l, op2, arg2h, arg2l, op3, arg3h, arg3l, op4, arg4h, arg4l]
 * Four operations are executed in sequence on the 16-bit seed.
 *
 * Set the active algorithm with seedkey_set_algo().
 * Compute a key from a seed with seedkey_compute().
 */

/* Set the active algorithm bytecodes (13 bytes). */
void seedkey_set_algo(const uint8_t *bytecodes);

/* Compute key from a 16-bit seed using the active algorithm. */
uint16_t seedkey_compute(uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif /* SEED_KEY_H */
