#ifndef KORSH_YP2_H
#define KORSH_YP2_H

#include <stdint.h>

/* 1 when the two-way kernel is compiled in (x86 with SSE2), 0 otherwise. */
int korsh_yp2_supported(void);

/*
 * Compute yespower 1.0 (N=256, r=8, no personalization) of two 80-byte block headers at once.
 * Returns 0 on success, -1 if the inputs are not supported (older timestamps use yespower 0.5): use the
 * one-at-a-time reference function in that case.
 */
int korsh_yespower_hash2(const uint8_t *in0, const uint8_t *in1, uint8_t *out0, uint8_t *out1);

#endif
