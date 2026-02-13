#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate minimal test EXR files for the gdk-pixbuf-exr test suite."""

import struct
import os

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")

def write_exr(path, width, height, pixel_data_rgb):
    """
    Write a minimal single-part scanline EXR file with FLOAT channels.

    EXR format (simplified for uncompressed scanline):
    - Magic: 0x762f3101 (4 bytes)
    - Version: 2 + scanline flag (4 bytes)
    - Header attributes (NUL-terminated name, NUL-terminated type, size, value)
    - End of header: NUL byte
    - Offset table: one int64 per scanline
    - Scanline data blocks
    """
    import io

    buf = io.BytesIO()

    # Magic number
    buf.write(struct.pack('<I', 0x01312f76))

    # Version field: version=2, single-part scanline
    # bit 9 (0x200) = tiled, we want 0 for scanline
    # bit 10-11: single part = 0
    buf.write(struct.pack('<I', 2))

    # Helper to write an attribute
    def write_attr(name, type_name, data):
        buf.write(name.encode('ascii') + b'\x00')
        buf.write(type_name.encode('ascii') + b'\x00')
        buf.write(struct.pack('<I', len(data)))
        buf.write(data)

    # channels attribute (chlist)
    # Each channel: name (NUL-terminated), pixel_type (int32), pLinear (uint8),
    #               reserved (3 bytes), xSampling (int32), ySampling (int32)
    ch_data = b''
    for ch_name in ['B', 'G', 'R']:  # EXR stores channels alphabetically
        ch_data += ch_name.encode('ascii') + b'\x00'
        ch_data += struct.pack('<I', 2)  # FLOAT = 2
        ch_data += struct.pack('<B', 0)  # pLinear
        ch_data += b'\x00' * 3  # reserved
        ch_data += struct.pack('<i', 1)  # xSampling
        ch_data += struct.pack('<i', 1)  # ySampling
    ch_data += b'\x00'  # end of channel list
    write_attr('channels', 'chlist', ch_data)

    # compression: 0 = NO_COMPRESSION
    write_attr('compression', 'compression', struct.pack('<B', 0))

    # dataWindow: box2i (4 x int32)
    write_attr('dataWindow', 'box2i',
               struct.pack('<iiii', 0, 0, width - 1, height - 1))

    # displayWindow: same as dataWindow
    write_attr('displayWindow', 'box2i',
               struct.pack('<iiii', 0, 0, width - 1, height - 1))

    # lineOrder: 0 = INCREASING_Y
    write_attr('lineOrder', 'lineOrder', struct.pack('<B', 0))

    # pixelAspectRatio: 1.0
    write_attr('pixelAspectRatio', 'float', struct.pack('<f', 1.0))

    # screenWindowCenter: v2f (2 x float)
    write_attr('screenWindowCenter', 'v2f', struct.pack('<ff', 0.0, 0.0))

    # screenWindowWidth: float
    write_attr('screenWindowWidth', 'float', struct.pack('<f', 1.0))

    # End of header
    buf.write(b'\x00')

    # Offset table: one uint64 per scanline
    # We need to compute where each scanline block starts.
    header_end = buf.tell()
    offset_table_size = height * 8  # 8 bytes per offset

    # Each scanline block: y_coordinate (int32) + pixel_data_size (int32) + pixel_data
    # For uncompressed: pixel_data_size = width * 3_channels * 4_bytes_per_float
    scanline_pixel_bytes = width * 3 * 4
    scanline_block_size = 4 + 4 + scanline_pixel_bytes  # y + size + data

    # Compute offsets
    data_start = header_end + offset_table_size
    for y in range(height):
        offset = data_start + y * scanline_block_size
        buf.write(struct.pack('<Q', offset))

    # Scanline data blocks
    # Channels are stored in alphabetical order: B, G, R
    for y in range(height):
        buf.write(struct.pack('<i', y))  # y coordinate
        buf.write(struct.pack('<I', scanline_pixel_bytes))  # data size

        # Write channel data: all B values, then all G values, then all R values
        for ch_idx in [2, 1, 0]:  # B=2, G=1, R=0 in the RGB input
            for x in range(width):
                pixel = pixel_data_rgb[y * width + x]
                buf.write(struct.pack('<f', pixel[ch_idx]))

    data = buf.getvalue()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def main():
    # simple.exr: 8x8 gradient image with varying colors
    width, height = 8, 8
    pixels = []
    for y in range(height):
        for x in range(width):
            r = (x + 1) / width * 2.0   # 0.25 to 2.0
            g = (y + 1) / height * 1.5  # 0.1875 to 1.5
            b = 0.5
            pixels.append((r, g, b))

    write_exr(os.path.join(DATA_DIR, "simple.exr"), width, height, pixels)
    print(f"Created simple.exr ({width}x{height})")

    # corrupt.exr: just some garbage bytes
    with open(os.path.join(DATA_DIR, "corrupt.exr"), 'wb') as f:
        f.write(b'\xde\xad\xbe\xef' * 16)
    print("Created corrupt.exr")

    # empty.exr: zero bytes
    with open(os.path.join(DATA_DIR, "empty.exr"), 'wb') as f:
        pass
    print("Created empty.exr")

    # not-an-exr.dat: PNG-like header
    with open(os.path.join(DATA_DIR, "not-an-exr.dat"), 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + b'\x00' * 64)
    print("Created not-an-exr.dat")


if __name__ == "__main__":
    main()
