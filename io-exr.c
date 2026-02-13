// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * io-exr.c — gdk-pixbuf loader module for OpenEXR files using TinyEXR.
 *
 * Loads EXR images (single-part), tonemaps from HDR to 8-bit sRGB via the
 * Reinhard global operator, and returns an RGBA GdkPixbuf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <tinyexr.h>

#include "tonemap.h"

/* Sanity limits to reject pathological files early. */
#define EXR_MAX_DIMENSION  8192
#define EXR_MAX_PIXELS     (64 * 1024 * 1024)   /* 64 Mpixels */
#define EXR_MAX_FILE_SIZE  (256 * 1024 * 1024)   /* 256 MB */

/* Context for incremental (progressive) loading. */
typedef struct {
    GByteArray                 *buffer;
    GdkPixbufModuleSizeFunc     size_func;
    GdkPixbufModulePreparedFunc prepared_func;
    GdkPixbufModuleUpdatedFunc  updated_func;
    gpointer                    user_data;
} ExrContext;

/* ------------------------------------------------------------------ */
/*  Core decoder: EXR bytes in memory -> GdkPixbuf                    */
/* ------------------------------------------------------------------ */

static GdkPixbuf *
decode_exr_from_memory(const guint8 *data, gsize length, GError **error)
{
    EXRVersion  version;
    EXRHeader   header;
    EXRImage    image;
    const char *exr_err  = NULL;
    float      *flat_rgb = NULL;
    uint8_t    *srgb_buf = NULL;
    GdkPixbuf  *pixbuf   = NULL;
    int         ret;
    int         header_initialized = 0;
    int         image_loaded       = 0;

    /* --- Stage 1: Parse and validate EXR version --- */

    ret = ParseEXRVersionFromMemory(&version, data, length);
    if (ret != TINYEXR_SUCCESS) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "Not a valid EXR file");
        return NULL;
    }

    if (version.multipart) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "Multipart EXR not supported");
        return NULL;
    }

    /* --- Stage 2: Parse header --- */

    InitEXRHeader(&header);
    header_initialized = 1;

    ret = ParseEXRHeaderFromMemory(&header, &version, data, length, &exr_err);
    if (ret != TINYEXR_SUCCESS) {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "Failed to parse EXR header: %s",
                    exr_err ? exr_err : "unknown error");
        if (exr_err)
            FreeEXRErrorMessage(exr_err);
        goto cleanup;
    }

    /* Request float output for every channel. */
    for (int i = 0; i < header.num_channels; i++)
        header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;

    /* --- Stage 3: Load pixel data --- */

    InitEXRImage(&image);

    ret = LoadEXRImageFromMemory(&image, &header, data, length, &exr_err);
    if (ret != TINYEXR_SUCCESS) {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "Failed to load EXR image: %s",
                    exr_err ? exr_err : "unknown error");
        if (exr_err)
            FreeEXRErrorMessage(exr_err);
        goto cleanup;
    }
    image_loaded = 1;

    /* --- Validate dimensions --- */

    int width  = image.width;
    int height = image.height;

    if (width <= 0 || height <= 0 ||
        width > EXR_MAX_DIMENSION || height > EXR_MAX_DIMENSION ||
        (uint64_t)width * (uint64_t)height > EXR_MAX_PIXELS) {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "EXR image dimensions out of range: %d x %d",
                    width, height);
        goto cleanup;
    }

    /* --- Identify R, G, B, A channel indices --- */

    int ch_r = -1, ch_g = -1, ch_b = -1, ch_a = -1;

    for (int i = 0; i < header.num_channels; i++) {
        if (strcmp(header.channels[i].name, "R") == 0)      ch_r = i;
        else if (strcmp(header.channels[i].name, "G") == 0)  ch_g = i;
        else if (strcmp(header.channels[i].name, "B") == 0)  ch_b = i;
        else if (strcmp(header.channels[i].name, "A") == 0)  ch_a = i;
    }

    if (ch_r < 0 || ch_g < 0 || ch_b < 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "EXR file missing required R, G, or B channel");
        goto cleanup;
    }

    /* Output always has 4 channels (RGBA) for the tonemapper.  If the
     * source has no alpha, we pass 3-channel input and the tonemapper
     * fills alpha = 255.  If the source has alpha, we pass 4-channel. */
    int out_channels = (ch_a >= 0) ? 4 : 3;

    /* --- Interleave planar channel data into a flat float buffer --- */

    size_t pixel_count = (size_t)width * (size_t)height;

    flat_rgb = (float *)calloc(pixel_count, (size_t)out_channels * sizeof(float));
    if (!flat_rgb) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Out of memory allocating float buffer");
        goto cleanup;
    }

    {
        const float *src_r = (const float *)image.images[ch_r];
        const float *src_g = (const float *)image.images[ch_g];
        const float *src_b = (const float *)image.images[ch_b];
        const float *src_a = (ch_a >= 0) ? (const float *)image.images[ch_a]
                                          : NULL;

        for (size_t i = 0; i < pixel_count; i++) {
            float *dst = flat_rgb + i * (unsigned)out_channels;
            dst[0] = src_r[i];
            dst[1] = src_g[i];
            dst[2] = src_b[i];
            if (src_a)
                dst[3] = src_a[i];
        }
    }

    /* --- Tonemap HDR -> 8-bit sRGB --- */

    srgb_buf = (uint8_t *)calloc(pixel_count, 4);
    if (!srgb_buf) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Out of memory allocating sRGB buffer");
        goto cleanup;
    }

    tonemap_reinhard(flat_rgb, srgb_buf, width, height, out_channels);

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
    free(flat_rgb);
    free(srgb_buf);
    if (image_loaded)
        FreeEXRImage(&image);
    if (header_initialized)
        FreeEXRHeader(&header);

    return pixbuf;
}

/* ------------------------------------------------------------------ */
/*  Atomic (whole-file) loader                                        */
/* ------------------------------------------------------------------ */

static GdkPixbuf *
exr_load(FILE *f, GError **error)
{
    GdkPixbuf *pixbuf = NULL;
    guint8    *buf    = NULL;
    long       size;

    if (fseek(f, 0, SEEK_END) != 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to seek in EXR file");
        return NULL;
    }

    size = ftell(f);
    if (size < 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to determine EXR file size");
        return NULL;
    }

    if (size > EXR_MAX_FILE_SIZE) {
        g_set_error(error, GDK_PIXBUF_ERROR,
                    GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                    "EXR file too large (%ld bytes, limit %d)",
                    size, EXR_MAX_FILE_SIZE);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to rewind EXR file");
        return NULL;
    }

    gsize file_size = (gsize)size;
    buf = (guint8 *)g_malloc(file_size);

    if (fread(buf, 1, file_size, f) != file_size) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_FAILED,
                            "Failed to read EXR file");
        g_free(buf);
        return NULL;
    }

    pixbuf = decode_exr_from_memory(buf, file_size, error);
    g_free(buf);
    return pixbuf;
}

/* ------------------------------------------------------------------ */
/*  Incremental (progressive) loader                                  */
/* ------------------------------------------------------------------ */

static gpointer
exr_begin_load(GdkPixbufModuleSizeFunc     size_func,
               GdkPixbufModulePreparedFunc  prepared_func,
               GdkPixbufModuleUpdatedFunc   updated_func,
               gpointer                     user_data,
               GError                     **error)
{
    ExrContext *ctx;

    (void)error;

    ctx = g_new0(ExrContext, 1);
    ctx->buffer        = g_byte_array_new();
    ctx->size_func     = size_func;
    ctx->prepared_func = prepared_func;
    ctx->updated_func  = updated_func;
    ctx->user_data     = user_data;

    return ctx;
}

static gboolean
exr_load_increment(gpointer      context,
                   const guchar *buf,
                   guint         size,
                   GError      **error)
{
    ExrContext *ctx = (ExrContext *)context;

    g_byte_array_append(ctx->buffer, buf, size);

    if (ctx->buffer->len > EXR_MAX_FILE_SIZE) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                            "EXR data exceeds maximum file size");
        return FALSE;
    }

    return TRUE;
}

static gboolean
exr_stop_load(gpointer context, GError **error)
{
    ExrContext *ctx    = (ExrContext *)context;
    GdkPixbuf  *pixbuf = NULL;
    gboolean    result = TRUE;

    pixbuf = decode_exr_from_memory(ctx->buffer->data,
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
/*  Module entry points                                               */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT void
fill_vtable(GdkPixbufModule *module)
{
    module->load           = exr_load;
    module->begin_load     = exr_begin_load;
    module->load_increment = exr_load_increment;
    module->stop_load      = exr_stop_load;
}

G_MODULE_EXPORT void
fill_info(GdkPixbufFormat *info)
{
    static const GdkPixbufModulePattern signature[] = {
        { "\x76\x2f\x31\x01", NULL, 100 },
        { NULL, NULL, 0 }
    };

    static const gchar *mime_types[] = { "image/x-exr", NULL };
    static const gchar *extensions[] = { "exr", NULL };

    info->name        = "exr";
    info->signature   = (GdkPixbufModulePattern *)signature;
    info->description = "OpenEXR image";
    info->mime_types  = (gchar **)mime_types;
    info->extensions  = (gchar **)extensions;
    info->flags       = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license     = "LGPL";
}
