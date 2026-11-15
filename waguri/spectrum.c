// SPDX-License-Identifier: LGPL-2.1-or-later
#include "spectrum.h"

#include <complex.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "audio/aframe.h"
#include "audio/format.h"
#include "mpv/client.h"

enum { window_size = 2048, hop_size = window_size / 2, display_scale = 10000 };
static const float tau = 6.28318530718f;
static atomic_bool active;
static atomic_uint generation;
static atomic_uint bands[WAGURI_SPECTRUM_BANDS];

// The frontend uses one mpv playback core. Each core thread owns its FFT input.
static _Thread_local float samples[window_size];
static _Thread_local float hann_window[window_size];
static _Thread_local bool window_ready;
static _Thread_local int sample_count;
static _Thread_local int sample_rate;
static _Thread_local unsigned current_generation;

MPV_EXPORT void waguri_spectrum_enable(bool enabled)
{
    atomic_store(&active, false);
    atomic_fetch_add(&generation, 1);
    for (int band = 0; band < WAGURI_SPECTRUM_BANDS; band++)
        atomic_store(&bands[band], 0);
    atomic_store(&active, enabled);
}

MPV_EXPORT void waguri_spectrum_read(float *output)
{
    for (int band = 0; band < WAGURI_SPECTRUM_BANDS; band++)
        output[band] = (float)atomic_load(&bands[band]) / display_scale;
}

static float read_sample(const uint8_t *data, int offset, int format)
{
    switch (format) {
    case AF_FORMAT_U8:     return (data[offset] - 128) / 128.0f;
    case AF_FORMAT_S16:    return ((const int16_t *)data)[offset] / 32768.0f;
    case AF_FORMAT_S32:    return ((const int32_t *)data)[offset] / 2147483648.0f;
    case AF_FORMAT_S64:    return ((const int64_t *)data)[offset] / 9223372036854775808.0f;
    case AF_FORMAT_FLOAT:  return ((const float *)data)[offset];
    case AF_FORMAT_DOUBLE: return ((const double *)data)[offset];
    default:              return 0;
    }
}

static void publish_spectrum(int rate)
{
    if (!window_ready) {
        for (int i = 0; i < window_size; i++)
            hann_window[i] = 0.5f - 0.5f * cosf(tau * i / (window_size - 1));
        window_ready = true;
    }

    float complex fft[window_size];
    int index = 0;
    for (int i = 0; i < window_size; i++) {
        fft[index] = hann_window[i] * samples[i];
        int bit = window_size / 2;
        while (bit && (index & bit)) {
            index ^= bit;
            bit >>= 1;
        }
        index ^= bit;
    }

    for (int size = 2; size <= window_size; size *= 2) {
        float angle = -tau / size;
        float complex step = cosf(angle) + I * sinf(angle);
        for (int start = 0; start < window_size; start += size) {
            float complex phase = 1;
            for (int i = start; i < start + size / 2; i++) {
                int other = i + size / 2;
                float complex first = fft[i];
                float complex second = fft[other] * phase;
                fft[i] = first + second;
                fft[other] = first - second;
                phase *= step;
            }
        }
    }

    float max_hz = fminf(16000, rate / 2.0f);
    if (max_hz <= 35 || current_generation != atomic_load(&generation))
        return;

    for (int band = 0; band < WAGURI_SPECTRUM_BANDS; band++) {
        float ratio = max_hz / 35;
        float lower = 35 * powf(ratio, (float)band / WAGURI_SPECTRUM_BANDS);
        float upper = 35 * powf(ratio, (float)(band + 1) / WAGURI_SPECTRUM_BANDS);
        int from = fmaxf(1, lower * window_size / rate);
        int to = fminf(window_size / 2, fmaxf(from + 1, ceilf(upper * window_size / rate)));
        float peak = 0;
        for (int bin = from; bin < to; bin++)
            peak = fmaxf(peak, cabsf(fft[bin]) / (window_size / 4));
        float level = 1 + (20 * log10f(fmaxf(peak, 0.0001f))) / 58;
        atomic_store(&bands[band], (unsigned)(display_scale * fminf(1, fmaxf(0, level))));
    }
}

void waguri_spectrum_feed(struct mp_aframe *frame)
{
    if (!atomic_load(&active))
        return;

    int format = mp_aframe_get_format(frame);
    int rate = mp_aframe_get_rate(frame);
    int channels = mp_aframe_get_channels(frame);
    uint8_t **data = mp_aframe_get_data_ro(frame);
    if (!data || !af_fmt_is_pcm(format) || rate <= 0 || channels <= 0)
        return;

    unsigned next_generation = atomic_load(&generation);
    if (sample_rate != rate || current_generation != next_generation) {
        sample_count = 0;
        sample_rate = rate;
        current_generation = next_generation;
    }

    bool planar = af_fmt_is_planar(format);
    format = af_fmt_from_planar(format);
    int count = mp_aframe_get_size(frame);
    for (int i = 0; i < count; i++) {
        // Use the first channel, avoiding phase cancellation in stereo mixes.
        samples[sample_count++] = read_sample(data[0], planar ? i : i * channels, format);
        if (sample_count == window_size) {
            publish_spectrum(rate);
            memmove(samples, samples + hop_size, (window_size - hop_size) * sizeof(samples[0]));
            sample_count = window_size - hop_size;
        }
    }
}
