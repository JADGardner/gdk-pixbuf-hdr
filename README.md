# gdk-pixbuf-exr

GDK-PixBuf loader plugin for OpenEXR (`.exr`) files.

## Overview

Enables GTK applications — Eye of GNOME, Nautilus file manager (thumbnails),
gThumb, and anything using GDK-PixBuf — to natively open OpenEXR images.

Since EXR is a high-dynamic-range format (32-bit float per channel) and
GDK-PixBuf works with 8-bit sRGB, the plugin applies Reinhard tonemapping with
automatic exposure (log-average luminance, key = 0.18) and proper sRGB gamma
correction.

Uses [TinyEXR](https://github.com/syoyo/tinyexr) for decoding — small,
dependency-free, and fast.

## Dependencies

- `libtinyexr-dev`
- `libgdk-pixbuf-2.0-dev`
- `meson` (>= 0.60)
- `ninja`

Ubuntu/Debian:

```
sudo apt install libtinyexr-dev libgdk-pixbuf-2.0-dev meson ninja-build
```

## Building

```
meson setup builddir
meson compile -C builddir
sudo meson install -C builddir
```

## Verification

```
gdk-pixbuf-query-loaders 2>/dev/null | grep exr
eog photo.exr
```

## Testing

```
meson test -C builddir
```

## How it works

The loader performs two-stage decoding: it validates the EXR header before
allocating pixel memory, preventing malicious files from triggering excessive
allocation. Pixel data is tonemapped using the Reinhard operator with
log-average luminance for automatic exposure, then converted through the proper
sRGB gamma curve (linear below 0.0031308, gamma 2.4 above).

## License

LGPL-2.1-or-later. See COPYING.
