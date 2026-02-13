// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * tonemap.h — Reinhard tonemapping with auto-exposure for HDR to 8-bit sRGB.
 *
 * All functions are static inline so this header can be included directly
 * without creating a separate compilation unit.
 */

#ifndef TONEMAP_H
#define TONEMAP_H

#include <stddef.h>
#include <stdint.h>
#include <math.h>

/* Tonemapping parameters */
#define TONEMAP_KEY   0.18f
#define TONEMAP_DELTA 1e-6f

/*
 * linear_to_srgb — Convert a linear-light value to sRGB gamma.
 *
 * Implements the official sRGB transfer function (IEC 61966-2-1).
 */
static inline float
linear_to_srgb(float c)
{
    if (c <= 0.0031308f)
        return 12.92f * c;
    else
        return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/*
 * tonemap_reinhard — Tonemap HDR float pixels to 8-bit sRGB using the
 *                    Reinhard global operator with auto-exposure.
 *
 * @rgb_in:        Input float pixel data, num_channels floats per pixel.
 * @srgb_out:      Output buffer, always 4 bytes (RGBA) per pixel.
 *                 Caller must allocate width * height * 4 bytes.
 * @width:         Image width in pixels.
 * @height:        Image height in pixels.
 * @num_channels:  Channels per input pixel (3 = RGB, 4 = RGBA).
 *
 * The algorithm runs in two passes:
 *   1. Compute log-average luminance across all valid pixels.
 *   2. Apply the Reinhard operator per-pixel, convert to sRGB, and write out.
 *
 * NaN/Inf values are treated as invalid and mapped to black.  This is
 * important for robustness when loading untrusted EXR files.
 */
static inline void
tonemap_reinhard(const float *rgb_in, uint8_t *srgb_out,
                 int width, int height, int num_channels)
{
    const size_t stride = (unsigned)num_channels;
    const size_t pixel_count = (size_t)width * (size_t)height;

    /* ---- Pass 1: Compute log-average luminance ---- */

    float  sum_log     = 0.0f;
    size_t valid_count = 0;

    for (size_t i = 0; i < pixel_count; i++) {
        const float *px = rgb_in + i * stride;

        float r = fmaxf(0.0f, px[0]);
        float g = fmaxf(0.0f, px[1]);
        float b = fmaxf(0.0f, px[2]);

        float L = 0.2126f * r + 0.7152f * g + 0.0722f * b;

        if (!isfinite(L) || L <= 0.0f)
            continue;

        sum_log += logf(L + TONEMAP_DELTA);
        valid_count++;
    }

    /* All-black or all-invalid image: output black, preserving alpha. */
    if (valid_count == 0) {
        for (size_t i = 0; i < pixel_count; i++) {
            uint8_t *out = srgb_out + i * 4;
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            if (num_channels == 4) {
                const float *px = rgb_in + i * stride;
                float a = fmaxf(0.0f, fminf(1.0f, px[3]));
                out[3] = (uint8_t)(a * 255.0f + 0.5f);
            } else {
                out[3] = 255;
            }
        }
        return;
    }

    float Lavg  = expf(sum_log / (float)valid_count);
    float scale = TONEMAP_KEY / fmaxf(Lavg, TONEMAP_DELTA);

    /* ---- Pass 2: Tonemap and convert each pixel ---- */

    for (size_t i = 0; i < pixel_count; i++) {
        const float *px  = rgb_in  + i * stride;
        uint8_t     *out = srgb_out + i * 4;

        float r = fmaxf(0.0f, px[0]);
        float g = fmaxf(0.0f, px[1]);
        float b = fmaxf(0.0f, px[2]);

        float L = 0.2126f * r + 0.7152f * g + 0.0722f * b;

        if (L <= 0.0f || !isfinite(L)) {
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            /* Alpha: use input alpha if available, otherwise fully opaque. */
            if (num_channels == 4) {
                float a = fmaxf(0.0f, fminf(1.0f, px[3]));
                out[3] = (uint8_t)(a * 255.0f + 0.5f);
            } else {
                out[3] = 255;
            }
            continue;
        }

        /* Reinhard global operator: L_mapped = (s*L) / (1 + s*L) */
        float L_scaled = scale * L;
        float L_mapped = L_scaled / (1.0f + L_scaled);

        /* Ratio preserves per-channel colour. Safe because L > 0 here. */
        float ratio = L_mapped / L;

        /* Map each colour channel through the ratio, then to sRGB. */
        float mapped_r = linear_to_srgb(r * ratio);
        float mapped_g = linear_to_srgb(g * ratio);
        float mapped_b = linear_to_srgb(b * ratio);

        /* Clamp to [0, 1] and quantize to 8-bit. */
        out[0] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, mapped_r)) * 255.0f + 0.5f);
        out[1] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, mapped_g)) * 255.0f + 0.5f);
        out[2] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, mapped_b)) * 255.0f + 0.5f);

        /* Alpha channel. */
        if (num_channels == 4) {
            float a = fmaxf(0.0f, fminf(1.0f, px[3]));
            out[3] = (uint8_t)(a * 255.0f + 0.5f);
        } else {
            out[3] = 255;
        }
    }
}

#endif /* TONEMAP_H */
