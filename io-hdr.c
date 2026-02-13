// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * io-hdr.c — gdk-pixbuf loader module for Radiance HDR (.hdr) files.
 *
 * Pure-C RGBE decoder.  Loads HDR images, tonemaps from HDR to 8-bit sRGB
 * via the Reinhard global operator, and returns an RGBA GdkPixbuf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "tonemap.h"

/* Sanity limits to reject pathological files early. */
#define HDR_MAX_DIMENSION   8192
#define HDR_MAX_PIXELS      (64 * 1024 * 1024)   /* 64 Mpixels */
#define HDR_MAX_FILE_SIZE   (256 * 1024 * 1024)   /* 256 MB */
#define HDR_MAX_HEADER_SIZE (64 * 1024)            /* 64 KB */

/* Context for incremental (progressive) loading. */
typedef struct {
    GByteArray                 *buffer;
    GdkPixbufModuleSizeFunc     size_func;
    GdkPixbufModulePreparedFunc prepared_func;
    GdkPixbufModuleUpdatedFunc  updated_func;
    gpointer                    user_data;
} HdrContext;

/* ------------------------------------------------------------------ */
/*  RGBE helpers                                                       */
/* ------------------------------------------------------------------ */

static inline void
rgbe_to_float(const uint8_t rgbe[4], float *r, float *g, float *b)
{
    if (rgbe[3] == 0) {
        *r = *g = *b = 0.0f;
    } else {
        float f = ldexpf(1.0f, (int)rgbe[3] - 128 - 8);
        *r = (float)rgbe[0] * f;
        *g = (float)rgbe[1] * f;
        *b = (float)rgbe[2] * f;
    }
}

/* ------------------------------------------------------------------ */
/*  Header parsing                                                     */
/* ------------------------------------------------------------------ */

/*
 * parse_hdr_header — Parse a Radiance HDR header from memory.
 *
 * Returns the byte offset where pixel data begins, or 0 on error.
 * Sets *width, *height, and *flip_vertical on success.
 */
static size_t
parse_hdr_header(const uint8_t *data, size_t length,
                 int *width, int *height, gboolean *flip_vertical,
                 GError **error)
{
    /* Check for magic */
    if (length < 11 ||
        (memcmp(data, "#?RADIANCE", 10) != 0 &&
         memcmp(data, "#?RGBE", 6) != 0)) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "Not a valid Radiance HDR file");
        return 0;
    }

    /* Scan header lines until blank line (end of header).
     * Enforce max header size. */
    size_t pos = 0;
    size_t header_end = 0;
    gboolean found_format = FALSE;

    while (pos < length && pos < HDR_MAX_HEADER_SIZE) {
        /* Find end of current line */
        size_t line_start = pos;
        while (pos < length && data[pos] != '\n')
            pos++;
        if (pos >= length)
            break;

        size_t line_len = pos - line_start;
        pos++; /* skip '\n' */

        /* Check for blank line (may have \r before \n) */
        if (line_len == 0 || (line_len == 1 && data[line_start] == '\r')) {
            header_end = pos;
            break;
        }

        /* Check FORMAT= line */
        if (line_len >= 7 && memcmp(data + line_start, "FORMAT=", 7) == 0) {
            /* Strip trailing \r if present */
            size_t val_start = line_start + 7;
            size_t val_len = line_len - 7;
            if (val_len > 0 && data[val_start + val_len - 1] == '\r')
                val_len--;

            if (val_len == 18 &&
                memcmp(data + val_start, "32-bit_rle_rgbe", 15) == 0) {
                found_format = TRUE;
            } else if (val_len >= 15 &&
                       memcmp(data + val_start, "32-bit_rle_xyze", 15) == 0) {
                g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                    GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
                                    "XYZE format Radiance files are not supported");
                return 0;
            }
            /* Accept format line even if value doesn't exactly match —
             * some writers emit slight variations.  The magic check is
             * the real gatekeeper. */
            found_format = TRUE;
        }

        /* EXPOSURE= header: ignored.  The tonemapper handles the full
         * dynamic range, so the exposure multiplier is not needed. */
    }

    if (header_end == 0) {
        if (pos >= HDR_MAX_HEADER_SIZE) {
            g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                "HDR header exceeds maximum size");
        } else {
            g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                "Unterminated HDR header");
        }
        return 0;
    }

    (void)found_format; /* accepted even without explicit FORMAT line */

    /* Parse resolution string — next line after blank line */
    size_t res_start = header_end;
    size_t res_end = res_start;
    while (res_end < length && data[res_end] != '\n')
        res_end++;
    if (res_end >= length) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "HDR file missing resolution string");
        return 0;
    }

    /* NUL-terminate the resolution line for sscanf */
    size_t res_len = res_end - res_start;
    char res_buf[128];
    if (res_len >= sizeof(res_buf))
        res_len = sizeof(res_buf) - 1;
    memcpy(res_buf, data + res_start, res_len);
    /* Strip trailing \r */
    if (res_len > 0 && res_buf[res_len - 1] == '\r')
        res_len--;
    res_buf[res_len] = '\0';

    int w = 0, h = 0;
    *flip_vertical = FALSE;

    if (sscanf(res_buf, "-Y %d +X %d", &h, &w) == 2) {
        /* Standard orientation — no flip needed */
    } else if (sscanf(res_buf, "+Y %d +X %d", &h, &w) == 2) {
        *flip_vertical = TRUE;
    } else {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "Unsupported HDR orientation: %s", res_buf);
        return 0;
    }

    if (w <= 0 || h <= 0 ||
        w > HDR_MAX_DIMENSION || h > HDR_MAX_DIMENSION ||
        (uint64_t)w * (uint64_t)h > HDR_MAX_PIXELS) {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "HDR image dimensions out of range: %d x %d", w, h);
        return 0;
    }

    *width = w;
    *height = h;

    /* Pixel data starts after the resolution line + '\n' */
    return res_end + 1;
}

/* ------------------------------------------------------------------ */
/*  RLE scanline decoder                                               */
/* ------------------------------------------------------------------ */

/*
 * decode_rle_scanline — Decode one new-style RLE scanline.
 *
 * Returns TRUE on success, FALSE on error.
 * *pos is updated to point past the consumed data.
 */
static gboolean
decode_rle_scanline(const uint8_t *data, size_t length, size_t *pos,
                    uint8_t *scanline, int width, GError **error)
{
    /* Each channel is RLE-encoded separately: R, G, B, E */
    for (int ch = 0; ch < 4; ch++) {
        int x = 0;
        while (x < width) {
            if (*pos >= length) {
                g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                    "HDR RLE data truncated");
                return FALSE;
            }

            uint8_t byte = data[*pos];
            (*pos)++;

            if (byte > 128) {
                /* Run: repeat next byte (byte - 128) times */
                int count = byte - 128;
                if (x + count > width) {
                    g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                        GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                        "HDR RLE run exceeds scanline width");
                    return FALSE;
                }
                if (*pos >= length) {
                    g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                        GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                        "HDR RLE data truncated");
                    return FALSE;
                }
                uint8_t val = data[*pos];
                (*pos)++;
                for (int i = 0; i < count; i++)
                    scanline[(x + i) * 4 + ch] = val;
                x += count;
            } else {
                /* Literal: copy next `byte` values */
                int count = byte;
                if (count == 0) {
                    g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                        GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                        "HDR RLE zero-length literal");
                    return FALSE;
                }
                if (x + count > width) {
                    g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                        GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                        "HDR RLE literal exceeds scanline width");
                    return FALSE;
                }
                if (*pos + (size_t)count > length) {
                    g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                        GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                        "HDR RLE data truncated");
                    return FALSE;
                }
                for (int i = 0; i < count; i++) {
                    scanline[(x + i) * 4 + ch] = data[*pos];
                    (*pos)++;
                }
                x += count;
            }
        }
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Core decoder: HDR bytes in memory -> GdkPixbuf                     */
/* ------------------------------------------------------------------ */

static GdkPixbuf *
decode_hdr_from_memory(const guint8 *data, gsize length, GError **error)
{
    float      *float_buf = NULL;
    uint8_t    *srgb_buf  = NULL;
    uint8_t    *scanline  = NULL;
    GdkPixbuf  *pixbuf    = NULL;
    int         width = 0, height = 0;
    gboolean    flip_vertical = FALSE;

    /* --- Parse header --- */

    size_t pixel_start = parse_hdr_header(data, length, &width, &height,
                                          &flip_vertical, error);
    if (pixel_start == 0)
        return NULL;

    /* --- Decode pixel data --- */

    size_t pixel_count = (size_t)width * (size_t)height;

    float_buf = (float *)calloc(pixel_count, 3 * sizeof(float));
    if (!float_buf) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Out of memory allocating float buffer");
        goto cleanup;
    }

    scanline = (uint8_t *)malloc((size_t)width * 4);
    if (!scanline) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Out of memory allocating scanline buffer");
        goto cleanup;
    }

    size_t pos = pixel_start;

    for (int y = 0; y < height; y++) {
        /* Determine output row (may be flipped) */
        int out_y = flip_vertical ? (height - 1 - y) : y;

        if (pos + 4 > length) {
            g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                "HDR pixel data truncated");
            goto cleanup;
        }

        /* Check for new-style RLE: starts with 0x02 0x02 + width as big-endian */
        if (data[pos] == 0x02 && data[pos + 1] == 0x02) {
            int rle_width = ((int)data[pos + 2] << 8) | (int)data[pos + 3];
            if (rle_width != width) {
                g_set_error(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "HDR RLE width mismatch: expected %d, got %d",
                            width, rle_width);
                goto cleanup;
            }
            pos += 4; /* skip RLE header */

            if (!decode_rle_scanline(data, length, &pos, scanline,
                                     width, error))
                goto cleanup;
        } else {
            /* Flat (uncompressed): 4 bytes per pixel */
            size_t needed = (size_t)width * 4;
            if (pos + needed > length) {
                g_set_error_literal(error, GDK_PIXBUF_ERROR,
                                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                                    "HDR pixel data truncated");
                goto cleanup;
            }
            memcpy(scanline, data + pos, needed);
            pos += needed;
        }

        /* Convert RGBE scanline to float RGB */
        for (int x = 0; x < width; x++) {
            float r, g, b;
            rgbe_to_float(scanline + x * 4, &r, &g, &b);

            float *dst = float_buf + ((size_t)out_y * (size_t)width + (size_t)x) * 3;
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
        }
    }

    free(scanline);
    scanline = NULL;

    /* --- Tonemap HDR -> 8-bit sRGB --- */

    srgb_buf = (uint8_t *)calloc(pixel_count, 4);
    if (!srgb_buf) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Out of memory allocating sRGB buffer");
        goto cleanup;
    }

    tonemap_reinhard(float_buf, srgb_buf, width, height, 3);

    /* --- Create GdkPixbuf (always RGBA, 8-bit) --- */

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!pixbuf) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to allocate GdkPixbuf");
        goto cleanup;
    }

    {
        int    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
        guchar *pixels   = gdk_pixbuf_get_pixels(pixbuf);

        for (int y = 0; y < height; y++)
            memcpy(pixels + y * rowstride,
                   srgb_buf + (size_t)y * (unsigned)width * 4,
                   (size_t)width * 4);
    }

cleanup:
    free(float_buf);
    free(srgb_buf);
    free(scanline);

    return pixbuf;
}

/* ------------------------------------------------------------------ */
/*  Atomic (whole-file) loader                                         */
/* ------------------------------------------------------------------ */

static GdkPixbuf *
hdr_load(FILE *f, GError **error)
{
    GdkPixbuf *pixbuf = NULL;
    guint8    *buf    = NULL;
    long       size;

    if (fseek(f, 0, SEEK_END) != 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to seek in HDR file");
        return NULL;
    }

    size = ftell(f);
    if (size < 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to determine HDR file size");
        return NULL;
    }

    if (size > HDR_MAX_FILE_SIZE) {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "HDR file too large (%ld bytes, limit %d)",
                    size, HDR_MAX_FILE_SIZE);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to rewind HDR file");
        return NULL;
    }

    gsize file_size = (gsize)size;
    buf = (guint8 *)g_malloc(file_size);

    if (fread(buf, 1, file_size, f) != file_size) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to read HDR file");
        g_free(buf);
        return NULL;
    }

    pixbuf = decode_hdr_from_memory(buf, file_size, error);
    g_free(buf);
    return pixbuf;
}

/* ------------------------------------------------------------------ */
/*  Incremental (progressive) loader                                   */
/* ------------------------------------------------------------------ */

static gpointer
hdr_begin_load(GdkPixbufModuleSizeFunc     size_func,
               GdkPixbufModulePreparedFunc  prepared_func,
               GdkPixbufModuleUpdatedFunc   updated_func,
               gpointer                     user_data,
               GError                     **error)
{
    HdrContext *ctx;

    (void)error;

    ctx = g_new0(HdrContext, 1);
    ctx->buffer        = g_byte_array_new();
    ctx->size_func     = size_func;
    ctx->prepared_func = prepared_func;
    ctx->updated_func  = updated_func;
    ctx->user_data     = user_data;

    return ctx;
}

static gboolean
hdr_load_increment(gpointer      context,
                   const guchar *buf,
                   guint         size,
                   GError      **error)
{
    HdrContext *ctx = (HdrContext *)context;

    g_byte_array_append(ctx->buffer, buf, size);

    if (ctx->buffer->len > HDR_MAX_FILE_SIZE) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "HDR data exceeds maximum file size");
        return FALSE;
    }

    return TRUE;
}

static gboolean
hdr_stop_load(gpointer context, GError **error)
{
    HdrContext *ctx    = (HdrContext *)context;
    GdkPixbuf  *pixbuf = NULL;
    gboolean    result = TRUE;

    pixbuf = decode_hdr_from_memory(ctx->buffer->data,
                                    ctx->buffer->len,
                                    error);
    if (!pixbuf) {
        result = FALSE;
        goto out;
    }

    {
        int width  = gdk_pixbuf_get_width(pixbuf);
        int height = gdk_pixbuf_get_height(pixbuf);

        if (ctx->size_func) {
            ctx->size_func(&width, &height, ctx->user_data);
            if (width <= 0 || height <= 0)
                goto out;  /* load cancelled by caller */
        }

        if (ctx->prepared_func)
            ctx->prepared_func(pixbuf, NULL, ctx->user_data);

        if (ctx->updated_func)
            ctx->updated_func(pixbuf, 0, 0, width, height, ctx->user_data);
    }

out:
    if (pixbuf)
        g_object_unref(pixbuf);
    g_byte_array_free(ctx->buffer, TRUE);
    g_free(ctx);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Module entry points                                                */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT void
fill_vtable(GdkPixbufModule *module)
{
    module->load           = hdr_load;
    module->begin_load     = hdr_begin_load;
    module->load_increment = hdr_load_increment;
    module->stop_load      = hdr_stop_load;
}

G_MODULE_EXPORT void
fill_info(GdkPixbufFormat *info)
{
    static const GdkPixbufModulePattern signature[] = {
        { "#?RADIANCE", NULL, 100 },
        { "#?RGBE", NULL, 100 },
        { NULL, NULL, 0 }
    };

    static const gchar *mime_types[] = { "image/vnd.radiance", NULL };
    static const gchar *extensions[] = { "hdr", "pic", NULL };

    info->name        = "hdr";
    info->signature   = (GdkPixbufModulePattern *)signature;
    info->description = "Radiance HDR image";
    info->mime_types  = (gchar **)mime_types;
    info->extensions  = (gchar **)extensions;
    info->flags       = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license     = "LGPL";
}
