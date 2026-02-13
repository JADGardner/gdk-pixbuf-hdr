// SPDX-License-Identifier: LGPL-2.1-or-later
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#ifndef TEST_DATA_DIR
#error "TEST_DATA_DIR must be defined"
#endif

static char *
test_path(const char *name)
{
    return g_build_filename(TEST_DATA_DIR, name, NULL);
}

/* ---- EXR tests ---- */

/* Basic load: valid EXR file loads successfully with correct dimensions */
static void
test_exr_load_basic(void)
{
    GError *error = NULL;
    char *path = test_path("simple.exr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(pb);
    g_assert_cmpint(gdk_pixbuf_get_width(pb), ==, 8);
    g_assert_cmpint(gdk_pixbuf_get_height(pb), ==, 8);
    g_assert_cmpint(gdk_pixbuf_get_n_channels(pb), ==, 4);  /* always RGBA */

    g_object_unref(pb);
    g_free(path);
}

/* Pixel values: loaded pixels should be non-zero for a non-black image */
static void
test_exr_pixel_values(void)
{
    GError *error = NULL;
    char *path = test_path("simple.exr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(pb);

    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    int n_channels = gdk_pixbuf_get_n_channels(pb);

    /* At least one pixel should have non-zero RGB values */
    gboolean found_nonzero = FALSE;
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);

    for (int y = 0; y < h && !found_nonzero; y++) {
        for (int x = 0; x < w && !found_nonzero; x++) {
            guchar *p = pixels + y * rowstride + x * n_channels;
            if (p[0] > 0 || p[1] > 0 || p[2] > 0)
                found_nonzero = TRUE;
        }
    }
    g_assert_true(found_nonzero);

    g_object_unref(pb);
    g_free(path);
}

/* Corrupt file: should fail gracefully */
static void
test_exr_corrupt_file(void)
{
    GError *error = NULL;
    char *path = test_path("corrupt.exr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_null(pb);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);
}

/* Empty file: should fail gracefully */
static void
test_exr_empty_file(void)
{
    GError *error = NULL;
    char *path = test_path("empty.exr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_null(pb);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);
}

/* Wrong format: a file with non-EXR magic should fail */
static void
test_exr_wrong_format(void)
{
    GError *error = NULL;
    char *path = test_path("not-an-exr.dat");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_null(pb);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);
}

/* ---- HDR tests ---- */

/* Basic load: valid HDR file loads successfully with correct dimensions */
static void
test_hdr_load_basic(void)
{
    GError *error = NULL;
    char *path = test_path("simple.hdr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(pb);
    g_assert_cmpint(gdk_pixbuf_get_width(pb), ==, 8);
    g_assert_cmpint(gdk_pixbuf_get_height(pb), ==, 8);
    g_assert_cmpint(gdk_pixbuf_get_n_channels(pb), ==, 4);  /* always RGBA */

    g_object_unref(pb);
    g_free(path);
}

/* RLE load: valid RLE-encoded HDR file loads successfully */
static void
test_hdr_load_rle(void)
{
    GError *error = NULL;
    char *path = test_path("simple-rle.hdr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(pb);
    g_assert_cmpint(gdk_pixbuf_get_width(pb), ==, 32);
    g_assert_cmpint(gdk_pixbuf_get_height(pb), ==, 8);

    /* Verify non-zero pixels */
    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    int n_channels = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    gboolean found_nonzero = FALSE;

    for (int y = 0; y < 8 && !found_nonzero; y++) {
        for (int x = 0; x < 32 && !found_nonzero; x++) {
            guchar *p = pixels + y * rowstride + x * n_channels;
            if (p[0] > 0 || p[1] > 0 || p[2] > 0)
                found_nonzero = TRUE;
        }
    }
    g_assert_true(found_nonzero);

    g_object_unref(pb);
    g_free(path);
}

/* Pixel values: loaded HDR pixels should be non-zero */
static void
test_hdr_pixel_values(void)
{
    GError *error = NULL;
    char *path = test_path("simple.hdr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(pb);

    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    int n_channels = gdk_pixbuf_get_n_channels(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);

    gboolean found_nonzero = FALSE;
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);

    for (int y = 0; y < h && !found_nonzero; y++) {
        for (int x = 0; x < w && !found_nonzero; x++) {
            guchar *p = pixels + y * rowstride + x * n_channels;
            if (p[0] > 0 || p[1] > 0 || p[2] > 0)
                found_nonzero = TRUE;
        }
    }
    g_assert_true(found_nonzero);

    g_object_unref(pb);
    g_free(path);
}

/* Corrupt file: garbage bytes should fail gracefully */
static void
test_hdr_corrupt_file(void)
{
    GError *error = NULL;
    char *path = test_path("corrupt.hdr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_null(pb);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);
}

/* Empty file: zero bytes should fail gracefully */
static void
test_hdr_empty_file(void)
{
    GError *error = NULL;
    char *path = test_path("empty.hdr");
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &error);

    g_assert_null(pb);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_free(path);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/exr/load-basic", test_exr_load_basic);
    g_test_add_func("/exr/pixel-values", test_exr_pixel_values);
    g_test_add_func("/exr/corrupt-file", test_exr_corrupt_file);
    g_test_add_func("/exr/empty-file", test_exr_empty_file);
    g_test_add_func("/exr/wrong-format", test_exr_wrong_format);

    g_test_add_func("/hdr/load-basic", test_hdr_load_basic);
    g_test_add_func("/hdr/load-rle", test_hdr_load_rle);
    g_test_add_func("/hdr/pixel-values", test_hdr_pixel_values);
    g_test_add_func("/hdr/corrupt-file", test_hdr_corrupt_file);
    g_test_add_func("/hdr/empty-file", test_hdr_empty_file);

    return g_test_run();
}
