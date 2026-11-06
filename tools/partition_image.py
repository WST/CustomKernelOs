"""Wrap existing raw MyFS volumes in a NEW MBR image; inputs are never modified.
Usage: python3 tools/partition_image.py OUTPUT ROOT_MYFS [SECOND_MYFS ...]
Root is the first valid MyFS primary partition at boot. Never run on live disks.
"""
import argparse
import pathlib
import shutil
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("volumes", nargs="+", type=pathlib.Path)
    args = parser.parse_args()
    if not 1 <= len(args.volumes) <= 4:
        parser.error("MBR supports one to four primary volumes")
    entries = []
    start = 2048
    for path in args.volumes:
        size = path.stat().st_size
        with path.open("rb") as src:
            magic = src.read(4)
        if size % 512 or size > 1024 * 1024 * 1024 or magic != b"MYFS":
            parser.error(f"{path}: expected a sector-aligned MyFS volume <= 1 GiB")
        entries.append((path, start, size // 512))
        start = (start + size // 512 + 2047) // 2048 * 2048
    mbr = bytearray(512)
    mbr[510:] = b"\x55\xaa"
    for i, (_, lba, sectors) in enumerate(entries):
        struct.pack_into("<B3sB3sII", mbr, 446 + i * 16,
                         0, b"\xfe\xff\xff", 0x83, b"\xfe\xff\xff", lba, sectors)
    # Exclusive creation prevents accidental replacement of a disk image.
    with args.output.open("xb") as dst:
        dst.truncate(start * 512)
        dst.write(mbr)
        for path, lba, _ in entries:
            dst.seek(lba * 512)
            with path.open("rb") as src:
                shutil.copyfileobj(src, dst)
    print(f"Created {args.output}: {len(entries)} MyFS primary partitions")


if __name__ == "__main__":
    main()
