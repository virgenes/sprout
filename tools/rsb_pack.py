#!/usr/bin/env python3
"""rsb_pack.py — Professional RSB/PGSR repacker for PvZ2 modding.

Re-packs modified game files back into a functional OBB (RSB archive).
Supports:
  - Patching individual files within a group
  - Full rebuild with modified/added files
  - Preserving original trie structure (no need to rebuild from scratch)
  - 4096-byte aligned file offsets (matching original format)

Usage:
  python rsb_pack.py original.obb modified_dir/ output.obb
"""

import json
import os
import shutil
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from rsb_unpack import RsbArchive, PgsrArchive, PGSR_MAGIC


PAGE_SIZE = 4096  # File offsets are page-aligned in the data section


def _rd32(data: bytes, off: int) -> int:
    return struct.unpack_from('<I', data, off)[0]


def _wr32(data: bytearray, off: int, val: int):
    struct.pack_into('<I', data, off, val)


def _rd24(data: bytes, off: int) -> int:
    return data[off] | (data[off + 1] << 8) | (data[off + 2] << 16)


def _align_up(val: int, align: int) -> int:
    return (val + align - 1) // align * align


class PgsrRepacker:
    """Repacks a single PGSR group blob with modified file data."""

    def __init__(self, original_blob: bytes):
        self.original = original_blob
        if original_blob[:4] != PGSR_MAGIC:
            raise ValueError("Not a PGSR blob")
        self._parse()

    def _parse(self):
        d = self.original
        self.data_off = _rd32(d, 0x14)
        self.list_len = _rd32(d, 0x48)
        self.list_beg = _rd32(d, 0x4C)
        self.total_size = len(d)

        # Decompress the data section
        payload = d[self.data_off:]
        self._dec_data = None
        self.was_compressed = False
        if len(payload) >= 2 and payload[0] == 0x78:
            try:
                self._dec_data = zlib.decompress(payload)
                self.was_compressed = True
            except zlib.error:
                pass
        if self._dec_data is None:
            self._dec_data = payload

        # Parse the name trie to find terminal nodes
        self.terminals: List[dict] = []
        stack = []
        name = ""
        pos = 0

        while pos + 4 <= self.list_len:
            ch = d[self.list_beg + pos]
            branch = _rd24(d, self.list_beg + pos + 1)

            if ch == 0:  # terminal node
                flag = _rd32(d, self.list_beg + pos + 4)
                off = _rd32(d, self.list_beg + pos + 8)
                size = _rd32(d, self.list_beg + pos + 12)
                meta_size = 16 + (20 if flag == 1 else 0)

                self.terminals.append({
                    'pos': pos,          # position relative to list_beg
                    'name': name,
                    'flag': flag,
                    'off': off,
                    'size': size,
                    'meta_size': meta_size,
                })

                pos += meta_size
                if stack:
                    name = stack[-1]['name']
                    pos = stack[-1]['pos']
                    stack.pop()
                else:
                    if not name:
                        break
                    name = ""
            else:
                if branch != 0:
                    stack.append({'name': name, 'pos': branch * 4})
                name += chr(ch)
                pos += 4

    def get_file_data(self, file_path: str) -> Optional[bytes]:
        """Get original data for a file path."""
        for t in self.terminals:
            if t['name'] == file_path:
                start = t['off']
                end = start + t['size']
                if end <= len(self._dec_data):
                    return bytes(self._dec_data[start:end])
                return None
        return None

    def rebuild(self, file_updates: Dict[str, bytes]) -> bytes:
        """Rebuild PGSR with updated file data. Returns new blob bytes.

        Args:
            file_updates: {file_path: new_data_bytes} — only changed files
        """
        # ── 1. Collect all file data (original + updates) ──
        new_data_map: Dict[str, bytes] = {}
        for t in self.terminals:
            path = t['name']
            if path in file_updates:
                new_data_map[path] = file_updates[path]
            else:
                start = t['off']
                end = start + t['size']
                if end <= len(self._dec_data):
                    new_data_map[path] = bytes(self._dec_data[start:end])
                else:
                    new_data_map[path] = b''

        # ── 2. Build new decompressed data section with page-aligned offsets ──
        new_dec = bytearray()
        new_off_map: Dict[str, int] = {}
        for t in self.terminals:
            data = new_data_map[t['name']]
            # Page-align the offset
            new_off = _align_up(len(new_dec), PAGE_SIZE)
            # Pad to alignment
            new_dec.extend(b'\x00' * (new_off - len(new_dec)))
            new_dec.extend(data)
            new_off_map[t['name']] = new_off

        # ── 3. Compress ──
        new_compressed = zlib.compress(bytes(new_dec)) if self.was_compressed else bytes(new_dec)

        # ── 4. Build new blob ──
        # Keep header (up to list_beg) + trie (unmodified except terminal metadata)
        trie_start = self.list_beg
        trie_end = self.list_beg + self.list_len
        header_raw = self.original[:trie_end]

        # Update terminal node metadata
        header_buf = bytearray(header_raw)
        for t in self.terminals:
            abs_pos = self.list_beg + t['pos']
            new_off = new_off_map[t['name']]
            new_sz = len(new_data_map[t['name']])
            _wr32(header_buf, abs_pos + 8, new_off)
            _wr32(header_buf, abs_pos + 12, new_sz)

        # Calculate new data offset — place right after trie
        new_data_off = _align_up(len(header_buf), PAGE_SIZE)
        # Pad header to alignment
        header_buf.extend(b'\x00' * (new_data_off - len(header_buf)))

        # Update header fields
        _wr32(header_buf, 0x14, new_data_off)
        _wr32(header_buf, 0x1c, len(new_compressed))
        _wr32(header_buf, 0x20, len(new_dec))
        _wr32(header_buf, 0x28, new_data_off + len(new_compressed))

        return bytes(header_buf) + new_compressed


class RsbPacker:
    """Repacks a complete RSB archive (OBB) with modified files."""

    def __init__(self, obb_path: str, output_path: str):
        self.obb_path = Path(obb_path)
        self.output_path = Path(output_path)
        if not self.obb_path.exists():
            raise FileNotFoundError(f"OBB not found: {obb_path}")
        self.rsb = RsbArchive(str(self.obb_path))

        # Read original RSB header
        with open(self.obb_path, 'rb') as f:
            self.original_header = f.read(self.rsb.full_header_size)

    def pack_group(self, group_name: str, file_updates: Dict[str, bytes]) -> bytes:
        """Repack a single group's PGSR with modified files."""
        group = self.rsb.groups_by_name[group_name]
        with open(self.obb_path, 'rb') as f:
            f.seek(group.offset)
            blob = f.read(group.size)

        if blob[:4] != PGSR_MAGIC:
            # Not a PGSR group — cannot repack (raw blob), return as-is
            return blob

        repacker = PgsrRepacker(blob)
        return repacker.rebuild(file_updates)

    def pack_all(self, modified_dir: Path, progress_cb=None) -> Path:
        """Repack all groups into a new OBB, applying modifications from directory.

        Args:
            modified_dir: Directory containing modified files organized by group.
            progress_cb: Optional callback(group_name, status).

        Returns:
            Path to the output OBB file.
        """
        t0 = time.time()

        # ── Scan modified files ──
        modified: Dict[str, Dict[str, bytes]] = {}
        if modified_dir.exists():
            for gf in modified_dir.rglob('*'):
                if gf.is_file():
                    rel = gf.relative_to(modified_dir)
                    group_name = rel.parts[0]
                    if group_name not in modified:
                        modified[group_name] = {}
                    inner = '/'.join(rel.parts[1:]) if len(rel.parts) > 1 else rel.parts[0]
                    modified[group_name][inner] = gf.read_bytes()

        # ── Rebuild all group blobs ──
        all_new_blobs: Dict[str, bytes] = {}
        current_offset = self.rsb.full_header_size

        with open(self.obb_path, 'rb') as f_orig:
            for g in self.rsb.groups:
                updates = modified.get(g.name, {})
                try:
                    if updates:
                        new_blob = self.pack_group(g.name, updates)
                    else:
                        f_orig.seek(g.offset)
                        new_blob = f_orig.read(g.size)
                except Exception as e:
                    if progress_cb:
                        progress_cb(g.name, f'ERR: {e}')
                    f_orig.seek(g.offset)
                    new_blob = f_orig.read(g.size)

                all_new_blobs[g.name] = new_blob
                if progress_cb:
                    progress_cb(g.name, 'OK')

        # ── Write output OBB with updated header ──
        header = bytearray(self.original_header)
        stride = self.rsb.file_stride
        base = self.rsb.file_base
        offset = self.rsb.full_header_size

        for i, g in enumerate(self.rsb.groups):
            blob = all_new_blobs[g.name]
            sz = len(blob)
            entry_off = base + i * stride
            _wr32(header, entry_off + 128, offset)
            _wr32(header, entry_off + 132, sz)
            offset += sz

        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.output_path, 'wb') as f_out:
            f_out.write(header)
            for g in self.rsb.groups:
                f_out.write(all_new_blobs[g.name])

        elapsed = time.time() - t0
        obb_size = self.output_path.stat().st_size
        print(f"\nRepacked: {len(self.rsb.groups)} groups, {obb_size:,} bytes in {elapsed:.1f}s",
              file=sys.stderr)
        return self.output_path

    def _get_group_blob(self, name: str, offset: int, size: int,
                        updates: Dict[str, bytes]) -> bytes:
        """Get or rebuild a group blob."""
        if updates:
            return self.pack_group(name, updates)
        with open(self.obb_path, 'rb') as f:
            f.seek(offset)
            return f.read(size)


def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="RSB/PGSR repacker — pack modified files into PvZ2 OBB"
    )
    parser.add_argument("obb", type=Path, help="Original OBB file")
    parser.add_argument("modified", type=Path,
                        help="Directory with modified files (organized by group)")
    parser.add_argument("output", type=Path, help="Output OBB file")
    parser.add_argument("--group", help="Only repack this specific group")
    parser.add_argument("--list-groups", action="store_true",
                        help="List groups with modified files")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be done without actually repacking")
    parser.add_argument("--verbose", action="store_true", help="Verbose output")
    args = parser.parse_args()

    if not args.obb.exists():
        print(f"error: {args.obb} not found", file=sys.stderr)
        sys.exit(1)

    packer = RsbPacker(str(args.obb), str(args.output))

    group_filter = [args.group] if args.group else None

    if args.list_groups:
        if args.modified.exists():
            for gf in sorted(args.modified.rglob('*')) if args.modified.exists() else []:
                if gf.is_file():
                    rel = gf.relative_to(args.modified)
                    print(rel.parts[0])
        return

    if args.dry_run:
        print(f"Would repack {args.obb} -> {args.output}", file=sys.stderr)
        print(f"Modified dir: {args.modified}", file=sys.stderr)
        if args.group:
            print(f"Group filter: {args.group}", file=sys.stderr)
        return

    def progress(name, status):
        if args.verbose:
            marker = '✓' if status == 'OK' else '✗'
            print(f"  {marker} {name}: {status}", file=sys.stderr)

    packer.pack_all(args.modified, group_filter=group_filter, progress_cb=progress)
    print(f"Written to {args.output}", file=sys.stderr)


if __name__ == '__main__':
    main()
