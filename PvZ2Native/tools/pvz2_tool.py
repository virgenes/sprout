#!/usr/bin/env python3
"""pvz2_tool.py — Professional PvZ2 OBB modding suite.

One command to extract, convert, repack, and validate the entire game data.

Usage:
  python pvz2_tool.py obb/ extract outdir/       # Extract all files
  python pvz2_tool.py outdir/ to-json             # Convert RTON -> JSON
  python pvz2_tool.py outdir/ to-rton             # Convert JSON -> RTON
  python pvz2_tool.py obb/ repack outdir/ new.obb # Repack modified files
  python pvz2_tool.py obb/ pipeline outdir/       # Full pipeline (all steps)

Commands:
  extract    Extract all files from OBB into group-sorted directories
  to-json    Convert all .RTON files to .json (in-place alongside RTON)
  to-rton    Convert all .json files back to .RTON
  repack     Build a new OBB from extracted + modified files
  pipeline   Extract + to-json + repack in one command
  info       Show OBB structure summary
"""

import argparse
import json
import shutil
import struct
import sys
import time
from pathlib import Path
from typing import Optional


# ─── Console ─────────────────────────────────────────────────────────────────

class Progress:
    """Simple progress reporter."""
    def __init__(self, total: int, label: str = ""):
        self.total = total
        self.label = label
        self.start = time.time()
        self.last = 0

    def tick(self, n: int = 1, status: str = ""):
        self.last += n
        pct = self.last / self.total * 100 if self.total else 0
        elapsed = time.time() - self.start
        if status:
            print(f"  [{elapsed:5.1f}s] {self.last:>6d}/{self.total:<6d} ({pct:5.1f}%) {status}",
                  file=sys.stderr)

    def done(self):
        elapsed = time.time() - self.start
        print(f"  Done in {elapsed:.1f}s ({self.last} items)", file=sys.stderr)


# ─── Commands ────────────────────────────────────────────────────────────────

def cmd_extract(obb_path: Path, out_dir: Path, args):
    """Extract all files from OBB into group-sorted directories."""
    sys.path.insert(0, str(Path(__file__).parent))

    out_dir.mkdir(parents=True, exist_ok=True)

    from rsb_unpack import RsbArchive
    print(f"Opening {obb_path}...", file=sys.stderr)
    t0 = time.time()
    rsb = RsbArchive(str(obb_path))
    print(f"  RSB v{rsb.version}, {len(rsb.groups)} groups, header {rsb.full_header_size:,} bytes",
          file=sys.stderr)

    total = 0
    groups_done = 0
    prog = Progress(len(rsb.groups), "extract")

    for g in rsb.groups:
        group_dir = out_dir / g.name
        group_dir.mkdir(parents=True, exist_ok=True)
        try:
            blob = rsb.extract_group_blob(g.name)
            from rsb_unpack import PgsrArchive
            pgsr = PgsrArchive(blob, g.offset, str(obb_path))
            count = pgsr.extract_all(group_dir)
            total += count
            groups_done += 1
            prog.tick(status=f"{g.name}: {count} files")
        except ValueError:
            blob = rsb.extract_group_blob(g.name)
            (group_dir / 'raw_blob.bin').write_bytes(blob)
            groups_done += 1
            prog.tick(status=f"{g.name}: raw blob {len(blob)} bytes")

    t1 = time.time()
    print(f"\nExtracted: {total} files from {groups_done} groups in {t1-t0:.1f}s",
          file=sys.stderr)
    return True


def cmd_to_json(out_dir: Path, args):
    """Convert all RTON files to JSON."""
    sys.path.insert(0, str(Path(__file__).parent))

    rton_files = sorted(out_dir.rglob('*.RTON')) + sorted(out_dir.rglob('*.rton'))
    if not rton_files:
        print("No RTON files found.", file=sys.stderr)
        return False

    print(f"Found {len(rton_files)} RTON files", file=sys.stderr)
    t0 = time.time()

    # Determine JSON formatting: --pretty wins, --compact wins over default
    compact = True
    pretty = False
    if hasattr(args, 'pretty') and args.pretty:
        compact = False
        pretty = True
    elif hasattr(args, 'compact') and args.compact:
        compact = True
        pretty = False

    from rton_parser import RtonReader

    ok = fail = 0
    prog = Progress(len(rton_files), "to-json")

    for rf in rton_files:
        rel = rf.relative_to(out_dir)
        try:
            data = rf.read_bytes()

            # Handle edge cases (nested PGSR, zlib-wrapped)
            if data[:4] not in (b'RTON',):
                # Try nested PGSR
                if data[:4] == b'pgsr':
                    from rsb_unpack import PgsrArchive
                    try:
                        pgsr = PgsrArchive(data, 0, None)
                        for inner in pgsr.files:
                            inner_data = pgsr._read_file_data(inner)
                            if inner_data and inner_data[:4] == b'RTON':
                                # Save inner file
                                inner_path = rf.parent / inner.name.replace('\\', '/').split('/')[-1]
                                inner_path.write_bytes(inner_data)
                                data = inner_data
                                break
                    except:
                        pass
                # Try zlib decompress
                if len(data) >= 2 and data[0] == 0x78:
                    import zlib
                    try:
                        dec = zlib.decompress(data)
                        if dec[:4] == b'RTON':
                            data = dec
                            rf.write_bytes(data)  # fix on disk
                    except:
                        pass

            if data[:4] != b'RTON':
                fail += 1
                prog.tick(status=f"SKIP (not RTON): {rel}")
                continue

            reader = RtonReader(data)
            root = reader._read_root()
            reader._skip_footer()

            out_file = (out_dir / 'rton_json' / str(rel)
                        .replace('.RTON', '.json').replace('.rton', '.json'))
            out_file.parent.mkdir(parents=True, exist_ok=True)
            with open(out_file, 'w', encoding='utf-8') as f:
                if pretty:
                    json.dump(root, f, ensure_ascii=False, indent=2)
                else:
                    json.dump(root, f, ensure_ascii=False, separators=(',', ':') if compact else None)
            ok += 1
            prog.tick()
        except Exception as e:
            fail += 1
            prog.tick(status=f"FAIL: {rel} ({e})")

    t1 = time.time()
    print(f"\nConverted: {ok} OK, {fail} FAILED in {t1-t0:.1f}s", file=sys.stderr)
    return fail == 0


def cmd_to_rton(out_dir: Path, args):
    """Convert all JSON files back to RTON."""
    sys.path.insert(0, str(Path(__file__).parent))

    json_dir = out_dir / 'rton_json'
    if not json_dir.exists():
        print(f"No rton_json directory at {json_dir}", file=sys.stderr)
        return False

    json_files = sorted(json_dir.rglob('*.json'))
    if not json_files:
        print("No JSON files found.", file=sys.stderr)
        return False

    print(f"Found {len(json_files)} JSON files", file=sys.stderr)
    t0 = time.time()

    from rton_writer import RtonWriter

    ok = fail = 0
    prog = Progress(len(json_files), "to-rton")

    for jf in json_files:
        rel = jf.relative_to(json_dir)
        try:
            with open(jf, 'r', encoding='utf-8') as f:
                root = json.load(f)
            writer = RtonWriter()
            rton_data = writer.encode(root)
            out_path = out_dir / 'rton_recode' / str(rel).replace('.json', '.RTON')
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(rton_data)
            ok += 1
            prog.tick()
        except Exception as e:
            fail += 1
            prog.tick(status=f"FAIL: {rel} ({e})")

    t1 = time.time()
    print(f"\nRe-encoded: {ok} OK, {fail} FAILED in {t1-t0:.1f}s", file=sys.stderr)
    return fail == 0


def cmd_repack(obb_path: Path, modified_dir: Path, output_path: Path, args):
    """Repack modified files into a new OBB."""
    sys.path.insert(0, str(Path(__file__).parent))

    if not modified_dir.exists():
        print(f"error: {modified_dir} not found", file=sys.stderr)
        return False

    print(f"Repacking {obb_path} -> {output_path}", file=sys.stderr)
    print(f"Modified dir: {modified_dir}", file=sys.stderr)
    t0 = time.time()

    from rsb_pack import RsbPacker

    def progress(name, status):
        pass  # quiet by default

    packer = RsbPacker(str(obb_path), str(output_path))
    result = packer.pack_all(modified_dir, progress_cb=progress)

    t1 = time.time()
    print(f"Written: {result} ({result.stat().st_size:,} bytes, {t1-t0:.1f}s)",
          file=sys.stderr)
    return True


def cmd_info(obb_path: Path, args):
    """Show OBB structure summary."""
    sys.path.insert(0, str(Path(__file__).parent))
    from rsb_unpack import RsbArchive

    rsb = RsbArchive(str(obb_path))
    print(f"RSB v{rsb.version}")
    print(f"Groups: {len(rsb.groups)}")
    print(f"Header: {rsb.full_header_size:,} bytes")
    print(f"File base: {rsb.file_base}")
    print(f"File stride: {rsb.file_stride}")

    # Summarize group types
    pgsr_count = sum(1 for g in rsb.groups if g.name.endswith('_Common') or '.' not in g.name)
    print(f"\n{'Group':<40s} {'Offset':>10s} {'Size':>10s} {'Files':>8s}")
    print(f"{'-'*40} {'-'*10} {'-'*10} {'-'*8}")

    for g in rsb.groups[:10]:
        try:
            blob = rsb.extract_group_blob(g.name)
            from rsb_unpack import PgsrArchive
            pgsr = PgsrArchive(blob, g.offset, str(obb_path))
            fcount = len(pgsr.files)
        except:
            fcount = 0
        print(f"{g.name:<40s} {g.offset:>10,} {g.size:>10,} {fcount:>8}")

    if len(rsb.groups) > 10:
        print(f"  ... and {len(rsb.groups) - 10} more groups")

    return True


def cmd_pipeline(obb_path: Path, out_dir: Path, args):
    """Full pipeline: extract + to-json + repack."""
    print(f"{'='*60}", file=sys.stderr)
    print("PVZ2 MODDING PIPELINE", file=sys.stderr)
    print(f"{'='*60}", file=sys.stderr)
    t0 = time.time()

    ok = cmd_extract(obb_path, out_dir, args)
    if not ok:
        print("Extract failed, aborting.", file=sys.stderr)
        return False

    # Pipeline uses compact JSON by default (faster, smaller)
    if not hasattr(args, 'pretty') or not args.pretty:
        args.compact = True
    ok = cmd_to_json(out_dir, args)
    if not ok:
        print("RTON conversion had failures.", file=sys.stderr)
        # Continue anyway for partial results

    # Optionally repack
    if args.repack:
        output_obb = args.repack
        ok = cmd_repack(obb_path, out_dir, output_obb, args)
    else:
        # Repack as proof of concept (no modifications)
        output_obb = out_dir.parent / (out_dir.name + '_repacked.obb')
        print(f"\n--- Repack verification ---", file=sys.stderr)
        ok = cmd_repack(obb_path, out_dir, output_obb, args)

    t1 = time.time()
    print(f"\n{'='*60}", file=sys.stderr)
    print(f"PIPELINE COMPLETE in {t1-t0:.1f}s", file=sys.stderr)
    print(f"{'='*60}", file=sys.stderr)
    return True


# ─── CLI ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="PvZ2 OBB Modding Suite — extract, convert, repack"
    )
    sub = parser.add_subparsers(dest='command', required=True)

    # extract
    p_extract = sub.add_parser('extract', help='Extract all files from OBB')
    p_extract.add_argument('obb', type=Path, help='Path to main.obb')
    p_extract.add_argument('outdir', type=Path, help='Output directory')

    # to-json
    p_json = sub.add_parser('to-json', help='Convert RTON to JSON')
    p_json.add_argument('outdir', type=Path, help='Extracted directory')
    p_json.add_argument('--pretty', action='store_true', help='Pretty-print JSON output')
    p_json.add_argument('--compact', action='store_true', help='Compact JSON output (default for pipeline)')

    # to-rton
    p_rton = sub.add_parser('to-rton', help='Convert JSON to RTON')
    p_rton.add_argument('outdir', type=Path, help='Extracted directory')

    # repack
    p_repack = sub.add_parser('repack', help='Repack into new OBB')
    p_repack.add_argument('obb', type=Path, help='Original OBB')
    p_repack.add_argument('modified', type=Path, help='Modified files dir')
    p_repack.add_argument('output', type=Path, help='Output OBB path')

    # info
    p_info = sub.add_parser('info', help='Show OBB structure')
    p_info.add_argument('obb', type=Path, help='Path to main.obb')

    # pipeline
    p_pipe = sub.add_parser('pipeline', help='Full pipeline (extract + convert + repack)')
    p_pipe.add_argument('obb', type=Path, help='Path to main.obb')
    p_pipe.add_argument('outdir', type=Path, help='Output directory')
    p_pipe.add_argument('--repack', type=Path, default=None,
                        help='Repack output OBB path (optional)')
    p_pipe.add_argument('--pretty', action='store_true',
                        help='Pretty-print JSON output (slower, larger)')

    args = parser.parse_args()

    try:
        if args.command == 'extract':
            success = cmd_extract(args.obb, args.outdir, args)
        elif args.command == 'to-json':
            success = cmd_to_json(args.outdir, args)
        elif args.command == 'to-rton':
            success = cmd_to_rton(args.outdir, args)
        elif args.command == 'repack':
            success = cmd_repack(args.obb, args.modified, args.output, args)
        elif args.command == 'info':
            success = cmd_info(args.obb, args)
        elif args.command == 'pipeline':
            success = cmd_pipeline(args.obb, args.outdir, args)
        else:
            parser.print_help()
            sys.exit(1)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)

    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
