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
import zlib
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

    __slots__ = ("name", "offset", "size", "compressed", "flags")

    def __init__(self, name: str, offset: int, size: int,
                 compressed: bool = False, flags: int = 0):
        self.name = name
        self.offset = offset
        self.size = size
        self.compressed = compressed
        self.flags = flags

    def __repr__(self):
        return f"PgsrFile({self.name!r}, offset={self.offset}, size={self.size})"


class PgsrArchive:
    """Inner pgsr archive reader — extracts individual files from a group blob.

    Format (reverse-engineered from sprout::RsbIndex::load):
      Header (0x60 bytes):
        +0x00: magic "pgsr"
        +0x04: version? (4 bytes, not used)
        +0x14: data_off (uint32) — offset of raw file payload relative to
               this pgsr blob start.  File data lives at data_off + entry_off.
        +0x48: list_len (uint32) — byte size of the name trie list
        +0x4C: list_beg (uint32) — absolute offset of the name trie list
               within this pgsr blob.

      Name trie (starting at list_beg):
        Each node is 4 bytes: [char (1)] [sibling_branch_offset (uint24 LE)].
        - char == 0 → terminal node.  Followed by:
            flag   (uint32 at +4):  0 = raw, 1 = zlib-compressed texture
            off   (uint32 at +8):   absolute offset of file data relative
                                     to blob offset `data_off`
            size  (uint32 at +12):  uncompressed file size
            If flag == 1, 20 extra bytes of image info follow the metadata.
          The node + metadata + optional image info = 16 or 36 bytes.
        - char != 0 → path character.  If branch_offset != 0, the current
          position is saved on a stack before jumping to branch_offset * 4
          to explore a sibling branch after the current path finishes.

      The full path is reconstructed by collecting characters as the trie
      is walked depth-first.

      Note: the file payload region (from `data_off` to end of blob) is
      zlib-compressed as a single block.  The `off` and `size` fields in
      each trie entry index into the *decompressed* payload.
    """

    def __init__(self, data: bytes, group_obb_offset: int = 0, obb_path: str = None):
        self.data = data
        self.group_obb_offset = group_obb_offset  # absolute OBB offset of this group
        self.obb_path = obb_path
        self.files: List[PgsrFile] = []
        self._dec_data: Optional[bytes] = None  # decompressed payload
        self._data_off: int = 0                  # data_off from header
        self._parse()

    @staticmethod
    def _rd32(data: bytes, off: int) -> int:
        if off + 4 > len(data):
            return 0
        return struct.unpack_from("<I", data, off)[0]

    @staticmethod
    def _rd24(data: bytes, off: int) -> int:
        if off + 3 > len(data):
            return 0
        return (data[off] |
                (data[off + 1] << 8) |
                (data[off + 2] << 16))

    def _parse(self):
        d = self.data
        if len(d) < 0x50:
            return

        magic = d[:4]
        if magic != PGSR_MAGIC:
            raise ValueError(f"Not a pgsr archive (magic={magic!r})")

        data_off = self._rd32(d, 0x14)
        self._data_off = data_off
        list_len = self._rd32(d, 0x48)
        list_beg = self._rd32(d, 0x4C)

        if list_len == 0 or list_beg == 0:
            return
        if list_beg + list_len > len(d):
            return

        # Decompress the payload region (data_off .. EOF) as a single zlib block.
        payload = d[data_off:]
        if len(payload) >= 2 and payload[0] == 0x78 and \
           ((0x78 * 256 + payload[1]) % 31) == 0:
            try:
                dec = zlib.decompress(payload)
                self._dec_data = dec
            except zlib.error:
                self._dec_data = None

        # Walk the trie at list_beg
        # `stack` holds (name-so-far, position relative to trie start)
        stack: list = []
        name = ""
        pos = 0

        while pos + 4 <= list_len:
            ch = d[list_beg + pos]
            branch = self._rd24(d, list_beg + pos + 1)

            if ch == 0:  # terminal node
                flag = self._rd32(d, list_beg + pos + 4)
                off = self._rd32(d, list_beg + pos + 8)
                file_size = self._rd32(d, list_beg + pos + 12)

                if name:
                    # Absolute offset within the OBB
                    abs_off = self.group_obb_offset + data_off + off
                    self.files.append(PgsrFile(
                        name, abs_off, file_size,
                        compressed=(flag == 1), flags=flag))

                # Advance past terminal + metadata
                pos += 16 + (20 if flag == 1 else 0)

                if stack:
                    name = stack[-1][0]
                    pos = stack[-1][1]
                    stack.pop()
                else:
                    if not name:
                        break
                    name = ""
            else:
                if branch != 0:
                    stack.append((name, branch * 4))
                name += chr(ch)
                pos += 4

    def _read_file_data(self, f: PgsrFile) -> Optional[bytes]:
        """Read file data from the decompressed payload, or fall back to OBB."""
        if self._dec_data is not None:
            # `off` relative to decompressed payload start
            dec_start = f.offset - self.group_obb_offset - self._data_off
            if 0 <= dec_start and dec_start + f.size <= len(self._dec_data):
                return self._dec_data[dec_start:dec_start + f.size]
        # Fall back: read from the raw blob / OBB
        blob_end = f.offset - self.group_obb_offset + f.size
        blob_start = f.offset - self.group_obb_offset
        if blob_start >= 0 and blob_end <= len(self.data):
            return self.data[blob_start:blob_end]
        if self.obb_path and f.offset > 0:
            import os
            if os.path.exists(self.obb_path):
                with open(self.obb_path, "rb") as obb_f:
                    obb_f.seek(f.offset)
                    return obb_f.read(f.size)
        return None

    def extract_file(self, name: str) -> Optional[bytes]:
        """Extract a single file by name."""
        from pathlib import PurePosixPath, PureWindowsPath
        for f in self.files:
            if (f.name == name or
                    PurePosixPath(f.name).name == name or
                    PureWindowsPath(f.name).name == name):
                return self._read_file_data(f)
        return None

    def extract_all(self, output_dir: Path, filter_pattern: str = None) -> int:
        """Extract all files to output_dir. Returns count."""
        import fnmatch
        count = 0
        for f_ in self.files:
            rel = f_.name.replace("\\", "/")
            if filter_pattern and not fnmatch.fnmatch(rel, filter_pattern):
                continue

            dest = output_dir / rel
            dest.parent.mkdir(parents=True, exist_ok=True)

            data = self._read_file_data(f_)
            if data is not None:
                dest.write_bytes(data)
                count += 1
            else:
                print(f"  WARN: {rel} at OBB offset {f_.offset} — cannot read",
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
            group = rsb.groups_by_name[gname]
            try:
                pgsr = PgsrArchive(blob, group.offset, str(args.obb))
                print(f"\n{gname}/ ({len(pgsr.files)} files):", file=sys.stderr)
                for f_ in pgsr.files:
                    print(f"  {f_.name:<60} {f_.size:>10} bytes"
                          f"{' [TEXTURE]' if f_.compressed else ''}")
            except ValueError as e:
                print(f"  {gname}: not a pgsr archive ({e})", file=sys.stderr)
                print(f"  Raw blob: {len(blob)} bytes at offset {group.offset}")
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
        group = rsb.groups_by_name[gname]

        if args.no_pgsr:
            (output / gname).write_bytes(blob)
            print(f"  Saved raw blob ({len(blob)} bytes)", file=sys.stderr)
            continue

        try:
            pgsr = PgsrArchive(blob, group.offset, str(args.obb))
        except ValueError as e:
            print(f"  Not a pgsr archive ({e}), saving raw blob", file=sys.stderr)
            (output / gname).write_bytes(blob)
            continue

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
