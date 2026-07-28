#!/usr/bin/env python3
"""Extracts named resource groups out of a PopCap "1bsr" (RSB) archive,
e.g. the .obb expansion file for PvZ2.

Format confirmed this session by cross-checking a real decompile of
ResStreamsManager::LoadRSB (sub_867740) and its header reader
(sub_864950) in libPVZ2.so against a raw hex dump of the real .obb --
not guessed. See PvZ2-Port-Analysis.md section 9.17 for the write-up.

Header layout (all fields little-endian uint32 unless noted):
  0x00  magic, 4 bytes: b"1bsr" (native byte order for this platform;
        sub_864950 checks this and only byte-swaps everything if it
        DOESN'T match "1bsr" directly -- our real .obb always matches)
  0x04  format version (4 in the file this was reverse engineered against;
        version>=4 selects the header-size field at 0x6C instead of 0x0C)
  0x0C  legacy/pre-v4 header size (present but unused for version>=4)
  0x28  file/group table: entry count
  0x2C  file/group table: base offset (relative to start of header)
  0x30  file/group table: entry stride in bytes (204 in this build)
  0x48  (only relevant if it differs from the file table -- in every file
        seen so far it's identical to the file table, i.e. one group per
        table slot) group table: count/base/stride at +0x48/0x4C/0x50
  0x6C  full header size in bytes (== byte offset where the first group's
        raw data begins; group offsets/sizes below are absolute file
        offsets, not relative to anything)

Each 204-byte table entry:
  +0x00  NUL-terminated ASCII group name (e.g. "AudioCommon",
         "__MANIFESTGROUP__"), rest of the 128-byte field zero-padded
  +0x80  (128) absolute file offset where this group's raw data begins
  +0x84  (132) size in bytes of this group's raw data
  +0x88  (136) the entry's own sequential index (== loop counter, not
         independently useful)
  +0xC4  (196) a flag (0/1 seen -- meaning not yet confirmed)
  +0xC8  (200) another per-entry field (meaning not yet confirmed --
         does NOT simply track the entry index)

IMPORTANT: what this script extracts is each GROUP's raw bytes as they
sit in the outer 1bsr container. Every group's data blob is itself
ANOTHER small archive (\"pgsr\" magic seen in the extracted
__MANIFESTGROUP__ blob) bundling the actual named files (e.g.
"PROPERTIES\\RESOURCES.RTON", found as readable UTF-16LE text near the
start of that blob). That inner "pgsr" format has NOT been reverse
engineered yet -- this script stops at "here are a group's raw bytes",
it does not unpack individual files out of a group.
"""
import struct
import sys
from pathlib import Path

HEADER_MAGIC = b"1bsr"
GROUP_NAME_FIELD_SIZE = 128


class RsbGroup:
    def __init__(self, name: str, offset: int, size: int, index: int, flag: int, field200: int):
        self.name = name
        self.offset = offset
        self.size = size
        self.index = index
        self.flag = flag
        self.field200 = field200

    def __repr__(self):
        return f"RsbGroup(name={self.name!r}, offset={self.offset}, size={self.size})"


class RsbArchive:
    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            magic = f.read(4)
            if magic != HEADER_MAGIC:
                raise ValueError(f"not an RSB file (magic={magic!r}, expected {HEADER_MAGIC!r})")
            f.seek(0)
            prefix = f.read(0x70)
            full_header_size = struct.unpack_from("<I", prefix, 0x6C)[0]
            f.seek(0)
            self.header = f.read(full_header_size)
        self.full_header_size = full_header_size
        self.version = struct.unpack_from("<I", self.header, 0x04)[0]
        self.file_count, self.file_base, self.file_stride = struct.unpack_from("<III", self.header, 0x28)
        self.groups = self._parse_groups()
        self.groups_by_name = {g.name: g for g in self.groups}

    def _parse_groups(self):
        groups = []
        for i in range(self.file_count):
            off = self.file_base + i * self.file_stride
            rec = self.header[off:off + self.file_stride]
            name = rec[0:GROUP_NAME_FIELD_SIZE].split(b"\x00", 1)[0].decode("latin1")
            g_offset, g_size, g_index = struct.unpack_from("<III", rec, 128)
            g_flag, g_field200 = struct.unpack_from("<II", rec, 196)
            groups.append(RsbGroup(name, g_offset, g_size, g_index, g_flag, g_field200))
        return groups

    def extract_group(self, name: str) -> bytes:
        g = self.groups_by_name[name]
        with open(self.path, "rb") as f:
            f.seek(g.offset)
            return f.read(g.size)


def main():
    if len(sys.argv) < 2:
        print("usage: rsb_extract.py <path-to-.obb> [group_name_to_extract] [output_file]")
        print("  no group_name: lists all group names")
        raise SystemExit(1)

    archive = RsbArchive(sys.argv[1])
    print(f"# {archive.path.name}: version={archive.version} header_size={archive.full_header_size} "
          f"groups={archive.file_count}", file=sys.stderr)

    if len(sys.argv) == 2:
        for g in archive.groups:
            print(f"{g.name}\toffset={g.offset}\tsize={g.size}")
        return

    group_name = sys.argv[2]
    blob = archive.extract_group(group_name)
    if len(sys.argv) >= 4:
        Path(sys.argv[3]).write_bytes(blob)
        print(f"wrote {len(blob)} bytes to {sys.argv[3]}", file=sys.stderr)
    else:
        sys.stdout.buffer.write(blob)


if __name__ == "__main__":
    main()
