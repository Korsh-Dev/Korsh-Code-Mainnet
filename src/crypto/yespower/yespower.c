/*-
 * Copyright 2018 Cryply team
 * Copyright 2018 Smartiecoin Core Developers
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file was originally written by Cryply team as part of the Cryply
 * coin.
 */
#include "yespower.h"
#include "sysendian.h"
#include "sha256.c"
#include "sha256.h"
#include "yespower-opt.c"

static const yespower_params_t korsh_yespower_0_5 = {YESPOWER_0_5, 4096, 32, "WaviBanana", 10};

/*
 * Korsh proof of work: YesPower 1.0 with N=256, r=8.
 *
 * The memory region is 128*N*r bytes, so 128*256*8 = 256 KB (plus about
 * 100 KB of S-boxes). This is the smallest configuration the algorithm
 * accepts (N >= 256, r >= 8) and it is what lets the chain be mined from L2/L3
 * on older CPUs and mobile phones instead of needing a 1 MB working set.
 */
static const yespower_params_t korsh_yespower_1_0 = {YESPOWER_1_0, 256, 8, NULL, 0};

int yespower_hash(const char *input, char *output)
{
    uint32_t time = le32dec(&input[68]);
    if (time > 1546539305) {
        return yespower_tls(input, 80, &korsh_yespower_1_0, (yespower_binary_t *) output);
    } else {
        return yespower_tls(input, 80 ,&korsh_yespower_0_5, (yespower_binary_t *) output);
    }
}
