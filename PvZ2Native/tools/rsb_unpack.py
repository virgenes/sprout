#!/usr/bin/env python3
"""rsb_unpack.py — Full PopCap RSB/1bsr archive extractor with inner pgsr support.

Extracts individual files from PvZ2 OBB expansion packs.

Outer format: "1bsr" (RSB container)
  - Groups named resources (AudioCommon, __MANIFESTGROUP__, etc.)
  - Each group is a raw data blob in the outer archive

Inner format: "pgsr" (PopCap Grouped Storage Resource)
  - Header + file table + raw file data
  - Found inside each RSB group blob
  - Contains the actual game assets: RTON configs, textures, sounds, etc.

Usage:
    python rsb_unpack.py path/to/main.obb                  # List all files
    python rsb_unpack.py path/to/main.obb output/           # Extract all
    python rsb_unpack.py path/to/main.obb output/ AudioCommon  # Extract one group
    python rsb_unpack.py path/to/main.obb output/ --filter *.RTON  # Filter by pattern
"""

import argparse
import json
import os
import struct
import sys
from pathlib import Path
from typing import List, Optional


# ─── RSB (Outer Archive) ─────────────────────────────────────────────────────

HEADER_MAGIC = b"1bsr"
GROUP_NAME_FIELD_SIZE = 128


class RsbGroup:
    """A group entry in the outer RSB container."""

    __slots__ = ("name", "offset", "size", "index", "flag", "field200")

    def __init__(self, name: str, offset: int, size: int, index: int, flag: int, field200: int):
        self.name = name
        self.offset = offset
        self.size = size
        self.index = index
        self.flag = flag
        self.field200 = field200

    def __repr__(self):
        return f"RsbGroup({self.name!r}, offset={self.offset}, size={self.size})"


class RsbArchive:
    """Outer RSB archive reader."""

    def __init__(self, path: Path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            magic = f.read(4)
            if magic != HEADER_MAGIC:
                raise ValueError(f"Not an RSB file (magic={magic!r})")

            f.seek(0)
            prefix = f.read(0x70)
            full_header_size = struct.unpack_from("<I", prefix, 0x6C)[0]
            f.seek(0)
            self.header = f.read(full_header_size)

        self.full_header_size = full_header_size
        self.version = struct.unpack_from("<I", self.header, 0x04)[0]
        self.file_count, self.file_base, self.file_stride = struct.unpack_from(
            "<III", self.header, 0x28
        )
        self.groups = self._parse_groups()
        self.groups_by_name = {g.name: g for g in self.groups}

    def _parse_groups(self) -> List[RsbGroup]:
        groups = []
        for i in range(self.file_count):
            off = self.file_base + i * self.file_stride
            rec = self.header[off:off + self.file_stride]
            name = rec[:GROUP_NAME_FIELD_SIZE].split(b"\x00", 1)[0].decode("latin1")
            g_off, g_sz, g_idx = struct.unpack_from("<III", rec, 128)
            g_flag, g_f200 = struct.unpack_from("<II", rec, 196)
            groups.append(RsbGroup(name, g_off, g_sz, g_idx, g_flag, g_f200))
        return groups

    def extract_group_blob(self, name: str) -> bytes:
        g = self.groups_by_name[name]
        with open(self.path, "rb") as f:
            f.seek(g.offset)
            return f.read(g.size)


# ─── PGSR (Inner Archive) ────────────────────────────────────────────────────

PGSR_MAGIC = b"pgsr"


class PgsrFile:
    """A single file entry within a pgsr archive."""

    __slots__ = ("name", "offset", "size", "compressed_size", "flags", "crc32")

    def __init__(self, name: str, offset: int, size: int,
                 compressed_size: int = 0, flags: int = 0, crc32: int = 0):
        self.name = name
        self.offset = offset
        self.size = size
        self.compressed_size = compressed_size or size
        self.flags = flags
        self.crc32 = crc32

    def __repr__(self):
        return f"PgsrFile({self.name!r}, offset={self.offset}, size={self.size})"


class PgsrArchive:
    """Inner pgsr archive reader — extracts individual files from a group blob."""

    def __init__(self, data: bytes):
        self.data = data
        self.files: List[PgsrFile] = []
        self._parse()

    def _parse(self):
        d = self.data
        if len(d) < 12:
            return

        magic = d[:4]
        if magic != PGSR_MAGIC:
            raise ValueError(f"Not a pgsr archive (magic={magic!r})")

        # Read header
        # Offsets reverse-engineered from group blobs:
        #   +0x00: magic "pgsr"
        #   +0x04: version? (seen: 2, 3)
        #   +0x08: file count
        #   +0x0C: header size (offset to file table / raw data)
        version = struct.unpack_from("<H", d, 4)[0]
        file_count = struct.unpack_from("<H", d, 6)[0]

        if version >= 3:
            file_count = struct.unpack_from("<I", d, 8)[0]
            header_size = struct.unpack_from("<I", d, 0x0C)[0]
            entry_size = 0x108  # 264 bytes per entry
            entry_name_len = 256
            name_offset_in_entry = 0
            offset_offset_in_entry = 0x100
            size_offset_in_entry = 0x104
            table_offset = 0x10
        else:
            # Version 2 (simpler format)
            header_size = struct.unpack_from("<H", d, 8)[0]
            entry_size = 0x104  # 260 bytes per entry
            entry_name_len = 256
            name_offset_in_entry = 0
            offset_offset_in_entry = 0x100
            size_offset_in_entry = 0x104
            table_offset = 0x0A
            # Check the actual header size
            if header_size < 0x10:
                header_size = 0x10

        if file_count == 0 or file_count > 10000:
            return

        # Parse file table
        for i in range(file_count):
            entry_off = table_offset + i * entry_size
            if entry_off + entry_size > len(d):
                break

            # Read name (null-terminated, up to 256 bytes)
            name_raw = d[entry_off + name_offset_in_entry:
                         entry_off + name_offset_in_entry + entry_name_len]
            name_end = name_raw.find(b"\x00")
            name = name_raw[:name_end].decode("utf-8", errors="replace") if name_end > 0 else ""

            if not name:
                continue

            # Read offset and size
            file_offset = struct.unpack_from("<I", d, entry_off + offset_offset_in_entry)[0]
            file_size = struct.unpack_from("<I", d, entry_off + size_offset_in_entry)[0]

            # Optional flags/compression fields
            compressed_size = file_size
            flags = 0
            if version >= 3 and entry_off + 0x108 + 8 <= len(d):
                compressed_size = struct.unpack_from("<I", d, entry_off + 0x108)[0]
                flags = struct.unpack_from("<I", d, entry_off + 0x10C)[0]

            self.files.append(PgsrFile(name, file_offset, file_size,
                                       compressed_size, flags))

    def extract_file(self, name: str) -> Optional[bytes]:
        """Extract a single file by name."""
        for f in self.files:
            if f.name == name or f.name.endswith("/" + name) or f.name.endswith("\\" + name):
                start = f.offset
                end = start + f.size
                if end <= len(self.data):
                    return self.data[start:end]
        return None

    def extract_all(self, output_dir: Path, filter_pattern: str = None) -> int:
        """Extract all files to output_dir. Returns count."""
        import fnmatch
        count = 0
        for f_ in self.files:
            # Normalise path
            rel = f_.name.replace("\\", "/")
            if filter_pattern and not fnmatch.fnmatch(rel, filter_pattern):
                continue

            dest = output_dir / rel
            dest.parent.mkdir(parents=True, exist_ok=True)

            start = f_.offset
            end = start + f_.size
            if end <= len(self.data):
                dest.write_bytes(self.data[start:end])
                count += 1
            else:
                print(f"  WARN: {rel} exceeds blob bounds ({end} > {len(self.data)})",
                      file=sys.stderr)
        return count


# ─── Main ────────────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(
        description="PopCap RSB/1bsr archive extractor with inner pgsr support"
    )
    parser.add_argument("obb", type=Path, help="Path to main.obb (RSB archive)")
    parser.add_argument("output", type=Path, nargs="?",
                        help="Output directory (omit to just list)")
    parser.add_argument("group", nargs="?", help="Extract only this group name")
    parser.add_argument("--filter", default=None,
                        help="File pattern filter (e.g. *.RTON, *.tga)")
    parser.add_argument("--json", type=Path, default=None,
                        help="Output file listing as JSON")
    parser.add_argument("--no-pgsr", action="store_true",
                        help="Don't parse inner pgsr, extract group blobs only")
    parser.add_argument("--list-groups", action="store_true",
                        help="List RSB groups and exit")
    return parser.parse_args()


def main():
    args = parse_args()

    if not args.obb.exists():
        print(f"error: {args.obb} not found", file=sys.stderr)
        sys.exit(1)

    print(f"Opening {args.obb}...", file=sys.stderr)
    rsb = RsbArchive(args.obb)
    print(f"  RSB version {rsb.version}, {len(rsb.groups)} groups, "
          f"header {rsb.full_header_size} bytes", file=sys.stderr)

    if args.list_groups:
        print(f"\n{'Group Name':<40} {'Offset':>12} {'Size':>12} {'Index':>6}", file=sys.stderr)
        print(f"{'─'*40} {'─'*12} {'─'*12} {'─'*6}", file=sys.stderr)
        for g in rsb.groups:
            print(f"  {g.name:<40} {g.offset:>12} {g.size:>12} {g.index:>6}", file=sys.stderr)
        return

    # Determine groups to process
    target_groups = [args.group] if args.group else [g.name for g in rsb.groups]

    if not args.output:
        # Just list files in specified groups
        for gname in target_groups:
            if gname not in rsb.groups_by_name:
                print(f"  Group '{gname}' not found", file=sys.stderr)
                continue
            blob = rsb.extract_group_blob(gname)
            try:
                pgsr = PgsrArchive(blob)
                print(f"\n{gname}/ ({len(pgsr.files)} files):", file=sys.stderr)
                for f_ in pgsr.files:
                    print(f"  {f_.name:<60} {f_.size:>10} bytes"
                          f"{' [compressed]' if f_.compressed_size != f_.size else ''}")
            except ValueError as e:
                print(f"  {gname}: not a pgsr archive ({e})", file=sys.stderr)
                print(f"  Raw blob: {len(blob)} bytes at offset {rsb.groups_by_name[gname].offset}")
        return

    # Extract
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    total_files = 0

    all_file_listing = []

    for gname in target_groups:
        if gname not in rsb.groups_by_name:
            print(f"  SKIP: group '{gname}' not found", file=sys.stderr)
            continue

        print(f"\nExtracting {gname}/...", file=sys.stderr)
        blob = rsb.extract_group_blob(gname)

        if args.no_pgsr:
            # Just save the raw blob
            (output / gname).write_bytes(blob)
            print(f"  Saved raw blob ({len(blob)} bytes)", file=sys.stderr)
            continue

        try:
            pgsr = PgsrArchive(blob)
        except ValueError as e:
            print(f"  Not a pgsr archive ({e}), saving raw blob", file=sys.stderr)
            (output / gname).write_bytes(blob)
            continue

        # Extract files
        group_out = output / gname
        count = pgsr.extract_all(group_out, args.filter)
        total_files += count
        print(f"  Extracted {count} files to {group_out}/", file=sys.stderr)

        for f_ in pgsr.files:
            all_file_listing.append({
                "group": gname,
                "name": f_.name,
                "size": f_.size,
                "offset": f_.offset,
            })

    print(f"\nTotal: {total_files} files extracted to {output}/", file=sys.stderr)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "obb": str(args.obb),
                "version": rsb.version,
                "groups": len(rsb.groups),
                "files": all_file_listing,
            }, f, indent=1)
        print(f"Listing written to {args.json}", file=sys.stderr)


if __name__ == "__main__":
    main()
