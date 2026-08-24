#!/usr/bin/env python3
"""Embed an MP4 as a C byte array for the freestanding harness.

Usage: scripts/make-video-header.py [video.mp4] [out.h]

The harness prefers an initrd passed with `qemu -initrd file.mp4`; the
generated header is only the embedded fallback so the kernel boots without
an external file. Keep videos small if you embed them.
"""
import pathlib
import sys

src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "test.mp4")
dst = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "test_mp4.h")
data = src.read_bytes()

with dst.open("w") as f:
    f.write(f"/* auto-generated from {src.name} ({len(data)} bytes) */\n"
            f"unsigned int test_mp4_len = {len(data)};\n"
            "unsigned char test_mp4[] = {\n")
    for i in range(0, len(data), 16):
        f.write(",".join(f"0x{b:02x}" for b in data[i:i+16]) + ",\n")
    f.write("};\n")
print(f"wrote {dst} ({dst.stat().st_size} bytes from {len(data)} input bytes)")
