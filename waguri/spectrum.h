// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <stdbool.h>

#define WAGURI_SPECTRUM_BANDS 32

#ifdef __cplusplus
extern "C" {
#endif

struct mp_aframe;
void waguri_spectrum_feed(struct mp_aframe *frame);
void waguri_spectrum_enable(bool enabled);
void waguri_spectrum_read(float *bands);

#ifdef __cplusplus
}
#endif
