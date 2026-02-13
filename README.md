# gdk-pixbuf-hdr

GDK-PixBuf loader plugins for OpenEXR (`.exr`) and Radiance HDR (`.hdr`) files.

(Previously named `gdk-pixbuf-exr`.)

## Overview

Enables GTK applications — Eye of GNOME, Nautilus file manager (thumbnails),
gThumb, and anything using GDK-PixBuf — to natively open OpenEXR and Radiance
HDR images.

Both formats are high-dynamic-range (32-bit float per channel for EXR, RGBE for
HDR) and GDK-PixBuf works with 8-bit sRGB, so the plugins apply Reinhard
tonemapping with automatic exposure (log-average luminance, key = 0.18) and
proper sRGB gamma correction.

- **EXR loader** — uses [TinyEXR](https://github.com/syoyo/tinyexr) for
  decoding (small, dependency-free, and fast).
- **HDR loader** — pure-C RGBE decoder, no external dependencies.

## Dependencies

- `libtinyexr-dev` (for EXR support)
- `libgdk-pixbuf-2.0-dev`
- `meson` (>= 0.60)
- `ninja`

Ubuntu/Debian:

```
sudo apt install libtinyexr-dev libgdk-pixbuf-2.0-dev meson ninja-build
```

## Building

```
meson setup builddir --prefix=/usr
meson compile -C builddir
sudo meson install -C builddir
```

## Verification

```
gdk-pixbuf-query-loaders 2>/dev/null | grep -E 'exr|hdr'
eog photo.exr
eog photo.hdr
```

## Testing

```
meson test -C builddir
```

## How it works

Both loaders validate the file header before allocating pixel memory, preventing
malicious files from triggering excessive allocation. Pixel data is tonemapped
using the Reinhard operator with log-average luminance for automatic exposure,
then converted through the proper sRGB gamma curve (linear below 0.0031308,
gamma 2.4 above).

The HDR loader supports both flat (uncompressed) and new-style RLE-encoded
Radiance files. The EXR loader handles single-part scanline EXR files via
TinyEXR.

## License

LGPL-2.1-or-later. See COPYING.
