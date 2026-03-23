#!/usr/bin/env python3
"""
Generate an Odroid Go .fw file (mkfw format) for RetroESP32.

The Odroid Go built-in firmware updater reads .fw files from the SD card
at /sd/odroid/firmware/. The .fw format contains a header, tile image,
partition entries with embedded binaries, and a CRC32 checksum.

The updater writes partitions SEQUENTIALLY starting after the factory
partition and rewrites the ESP32 partition table to match.

Usage:
    python mkfw.py

Output:
    Firmware/Releases/RetroESP32.fw

Place the file on SD card at: /sd/odroid/firmware/RetroESP32.fw
"""

import struct
import os
import sys

# ---------- Configuration ----------

DESCRIPTION = "RetroESP32 v3.15"
TILE_PATH = "Emulators/retro-go/assets/tile.raw"
BINS_DIR = "Firmware/Bins"
OUTPUT = "Firmware/Releases/RetroESP32.fw"

# Partition entries: (type, subtype, partition_size, label, binary_filename)
# type 0 = APP, subtype 16+ = ota_0, ota_1, ...
# partition_size must match partitions.csv so the updater's sequential layout
# places each binary at the right distance from the previous one.
PARTITIONS = [
    (0, 0x10, 0x080000, "launcher",  "retro-esp32.bin"),    # ota_0
    (0, 0x11, 0x0C0000, "nes",       "nesemu-go.bin"),      # ota_1
    (0, 0x12, 0x0B0000, "gb",        "gnuboy-go.bin"),      # ota_2
    (0, 0x13, 0x160000, "sms",       "smsplusgx-go.bin"),   # ota_3
    (0, 0x14, 0x090000, "spectrum",  "spectrum.bin"),        # ota_4
    (0, 0x15, 0x1A0000, "a26",       "stella-go.bin"),      # ota_5
    (0, 0x16, 0x0C0000, "a78",       "prosystem-go.bin"),   # ota_6
    (0, 0x17, 0x0F0000, "lnx",       "handy-go.bin"),       # ota_7
    (0, 0x18, 0x0C0000, "pce",       "pcengine-go.bin"),    # ota_8
    (0, 0x19, 0x080000, "tyrian",    "OpenTyrian.bin"),     # ota_9
    (0, 0x1A, 0x0C0000, "a800",      "atari800-go.bin"),   # ota_10
]

# ---------- CRC32 (same as zlib crc32, matching ESP-IDF crc32_le) ----------

def crc32(data, prev=0):
    import zlib
    return zlib.crc32(data, prev) & 0xFFFFFFFF

# ---------- Main ----------

def main():
    base = os.path.dirname(os.path.abspath(__file__))
    tile_path = os.path.join(base, TILE_PATH)
    bins_dir = os.path.join(base, BINS_DIR)
    output = os.path.join(base, OUTPUT)

    # Header: "ODROIDGO_FIRMWARE_V00_01" (24 bytes, no null terminator)
    header = b"ODROIDGO_FIRMWARE_V00_01"

    # Description: 40 bytes, null-padded
    desc = DESCRIPTION.encode("ascii")[:40]
    desc = desc + b'\x00' * (40 - len(desc))

    # Tile: 86*48*2 = 8256 bytes RGB565
    if not os.path.isfile(tile_path):
        print(f"ERROR: Tile not found: {tile_path}")
        sys.exit(1)
    with open(tile_path, "rb") as f:
        tile = f.read()
    if len(tile) != 8256:
        print(f"ERROR: Tile must be 8256 bytes, got {len(tile)}")
        sys.exit(1)

    # Build firmware blob
    fw = bytearray()
    fw += header
    fw += desc
    fw += tile

    total_flash = 0
    for ptype, subtype, part_size, label, binfile in PARTITIONS:
        binpath = os.path.join(bins_dir, binfile)
        if not os.path.isfile(binpath):
            print(f"ERROR: Binary not found: {binpath}")
            sys.exit(1)

        with open(binpath, "rb") as f:
            data = f.read()

        if len(data) > part_size:
            print(f"ERROR: {binfile} ({len(data)} bytes) exceeds partition size ({part_size} bytes)")
            sys.exit(1)

        # odroid_partition_t: type(1) subtype(1) reserved(2) label(16) flags(4) length(4) = 28 bytes
        label_bytes = label.encode("ascii")[:16]
        label_bytes = label_bytes + b'\x00' * (16 - len(label_bytes))

        part_entry = struct.pack("<BB2s16sII",
            ptype, subtype, b'\x00\x00', label_bytes, 0, part_size)

        # Data length (actual binary size)
        data_len = struct.pack("<I", len(data))

        fw += part_entry
        fw += data_len
        fw += data

        total_flash += part_size
        print(f"  [{label:16s}] type={ptype} subtype=0x{subtype:02x} "
              f"part_size=0x{part_size:06x} data={len(data):,} bytes")

    # CRC32 checksum over entire content
    checksum = crc32(bytes(fw))
    fw += struct.pack("<I", checksum)

    # Write output
    os.makedirs(os.path.dirname(output), exist_ok=True)
    with open(output, "wb") as f:
        f.write(fw)

    print(f"\nGenerated: {output}")
    print(f"  Size: {len(fw):,} bytes ({len(fw)/1024/1024:.1f} MB)")
    print(f"  Partitions: {len(PARTITIONS)}")
    print(f"  Total flash usage: 0x{total_flash:X} ({total_flash/1024/1024:.1f} MB)")
    print(f"  CRC32: 0x{checksum:08X}")
    print(f"\nPlace on SD card at: /sd/odroid/firmware/RetroESP32.fw")

if __name__ == "__main__":
    main()
