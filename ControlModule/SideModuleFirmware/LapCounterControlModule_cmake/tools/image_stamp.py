#!/usr/bin/env python3
"""Fill size, loadAddr and imageCrc into the image header of a control module app ELF.

    python3 tools/image_stamp.py <app.elf> <arm-none-eabi-objcopy>

Run as a POST_BUILD step (cmake/ctrl_app.cmake). Writes <app>.bin next to the
ELF and patches the .image_header section of the ELF itself, so flashing
either one gives a valid header (image_header.h: header at +0x400, 32 bytes,
size = .bin length, loadAddr = .vectors address, imageCrc = CRC-32/zlib over
the image with the header skipped). The web app's
updater sends the .bin; UPDATE_BEGIN.crc32 is zlib.crc32 of the whole .bin.
"""

import struct
import subprocess
import sys
import zlib
from pathlib import Path

HEADER_OFFSET = 0x400
HEADER_BYTES = 32
MAGIC = 0x4D495454


def vectors_addr(elf):
    """Address of the .vectors section (the image's link address)."""
    d = elf.read_bytes()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        sys.exit("image_stamp: not an ELF32 file")
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    sec = [struct.unpack_from("<IIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = sec[shstrndx][4]
    for name, _type, _flags, addr, _off, _size in sec:
        end = d.index(b"\0", stroff + name)
        if d[stroff + name:end] == b".vectors":
            return addr
    sys.exit("image_stamp: no .vectors section")


def main():
    elf, objcopy = Path(sys.argv[1]), sys.argv[2]
    binf = elf.with_suffix(".bin")
    subprocess.run([objcopy, "-O", "binary", str(elf), str(binf)], check=True)
    img = bytearray(binf.read_bytes())
    if len(img) < HEADER_OFFSET + HEADER_BYTES:
        sys.exit("image_stamp: image shorter than its header")
    hdr = bytearray(img[HEADER_OFFSET:HEADER_OFFSET + HEADER_BYTES])
    if struct.unpack_from("<I", hdr, 0)[0] != MAGIC:
        sys.exit("image_stamp: no image header at +0x400")
    load = vectors_addr(elf)
    struct.pack_into("<I", hdr, 12, len(img))
    struct.pack_into("<I", hdr, 24, load)
    img[HEADER_OFFSET:HEADER_OFFSET + HEADER_BYTES] = hdr
    crc = zlib.crc32(img[:HEADER_OFFSET] + img[HEADER_OFFSET + HEADER_BYTES:]) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 16, crc)
    img[HEADER_OFFSET:HEADER_OFFSET + HEADER_BYTES] = hdr
    binf.write_bytes(img)
    hdrf = elf.with_suffix(".hdr")
    hdrf.write_bytes(hdr)
    subprocess.run([objcopy, "--update-section", f".image_header={hdrf}", str(elf)], check=True)
    print(f"image_stamp: {binf.name} loadAddr=0x{load:08x} size={len(img)} imageCrc=0x{crc:08x} fileCrc=0x{zlib.crc32(img) & 0xFFFFFFFF:08x}")


if __name__ == "__main__":
    main()
