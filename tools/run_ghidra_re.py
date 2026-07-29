#!/usr/bin/env python3
"""run_ghidra_re.py — Run all Ghidra reverse-engineering scripts in sequence.

This orchestrates the PyGhidra-based analysis tools:
  1. ghidra_jni.py      — Extract JNINativeMethod tables → symbols.cpp entry + JSON
  2. ghidra_globals.py   — Find LawnApp, AndroidAppDriver + other BSS globals → JSON
  3. ghidra_vtables.py   — Scan vtable hierarchy → JSON

Usage:
    python run_ghidra_re.py path/to/libPVZ2.so [--ghidra-dir DIR] [--out-dir DIR] [--name "X.Y.Z"]

Output:
    out/
      methods.json        — All JNI methods found
      methods_symbols.txt — kVersions entry for symbols.cpp
      globals.json        — BSS global variables
      vtables.json        — Vtable listings
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


SCRIPTS_DIR = Path(__file__).parent


def parse_args():
    parser = argparse.ArgumentParser(description="Run all Ghidra RE scripts")
    parser.add_argument("libpvz2", type=Path, help="Path to libPVZ2.so")
    parser.add_argument("--ghidra-dir", default=None,
                        help="Ghidra install directory")
    parser.add_argument("--out-dir", type=Path, default=Path("ghidra_out"),
                        help="Output directory (default: ghidra_out/)")
    parser.add_argument("--name", default=None,
                        help="Version name (auto-detected from file if not given)")
    parser.add_argument("--skip-analysis", action="store_true",
                        help="Skip Ghidra auto-analysis")
    return parser.parse_args()


def detect_version(so_path: Path) -> str:
    """Try to determine the version from known fingerprints."""
    import struct
    with open(so_path, "rb") as f:
        data = f.read()

    # Compare fingerprints from symbols.cpp
    known_fingerprints = [
        ("1.6.10", 0x9F1944, 0xE59F0014E92D4800),  # onDrawFrame
        ("4.5.2",  0xCC7E60, 0xE59F1010E59F0010),  # onDrawFrame
    ]

    for ver, offset, expected in known_fingerprints:
        if offset + 8 <= len(data):
            fp = struct.unpack_from("<Q", data, offset)[0]
            if fp == expected:
                return ver
    return "X.Y.Z"


def run_script(script_name: str, args_list: list, env: dict = None) -> bool:
    """Run a Python script and return True on success."""
    script_path = SCRIPTS_DIR / script_name
    if not script_path.exists():
        print(f"ERROR: {script_path} not found", file=sys.stderr)
        return False

    cmd = [sys.executable or "python", str(script_path)] + args_list

    print(f"\n{'='*70}", file=sys.stderr)
    print(f"  Running: {script_name}", file=sys.stderr)
    print(f"{'='*70}", file=sys.stderr)

    start = time.time()
    result = subprocess.run(cmd, env=env, capture_output=False)
    elapsed = time.time() - start

    if result.returncode == 0:
        print(f"  ✓ {script_name} completed in {elapsed:.1f}s", file=sys.stderr)
        return True
    else:
        print(f"  ✗ {script_name} FAILED (rc={result.returncode})", file=sys.stderr)
        return False


def main():
    args = parse_args()

    if not args.libpvz2.exists():
        print(f"error: {args.libpvz2} not found", file=sys.stderr)
        sys.exit(1)

    so_path = args.libpvz2.resolve()
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    version = args.name or detect_version(so_path)

    # Build common args
    ghidra_arg = []
    if args.ghidra_dir:
        ghidra_arg = ["--ghidra-dir", args.ghidra_dir]

    no_analysis = ["--no-analysis"] if args.skip_analysis else []

    # ── Step 1: ghidra_jni.py ───────────────────────────────────────────
    methods_json = out_dir / "methods.json"
    symbols_txt = out_dir / "methods_symbols.txt"

    success = run_script("ghidra_jni.py", [
        str(so_path),
        "--name", version,
        "--json", str(methods_json),
    ] + ghidra_arg + no_analysis, env={**os.environ, "PYTHONUNBUFFERED": "1"})

    if not success:
        print("FATAL: ghidra_jni.py failed — cannot continue", file=sys.stderr)
        sys.exit(1)

    # Save symbols output
    print(f"\n  [Output saved to {symbols_txt}]", file=sys.stderr)

    # ── Step 2: ghidra_globals.py ───────────────────────────────────────
    globals_json = out_dir / "globals.json"

    run_script("ghidra_globals.py", [
        str(so_path),
        "--method-json", str(methods_json),
        "--json", str(globals_json),
    ] + ghidra_arg + no_analysis, env={**os.environ, "PYTHONUNBUFFERED": "1"})

    # ── Step 3: ghidra_vtables.py ───────────────────────────────────────
    vtables_json = out_dir / "vtables.json"

    run_script("ghidra_vtables.py", [
        str(so_path),
        "--json", str(vtables_json),
        "--min-vtable", "5",
    ] + ghidra_arg + no_analysis, env={**os.environ, "PYTHONUNBUFFERED": "1"})

    # ── Summary ─────────────────────────────────────────────────────────
    print(f"\n{'='*70}", file=sys.stderr)
    print(f"  ANALYSIS COMPLETE", file=sys.stderr)
    print(f"{'='*70}", file=sys.stderr)
    print(f"  Version:       {version}", file=sys.stderr)
    print(f"  Library:       {so_path}", file=sys.stderr)
    print(f"  Output:        {out_dir}/", file=sys.stderr)
    print(file=sys.stderr)

    for f in out_dir.iterdir():
        if f.is_file():
            size = f.stat().st_size
            print(f"    {f.name:30s} {size:>8,} bytes", file=sys.stderr)

    print(file=sys.stderr)
    print("  To integrate: copy methods_symbols.txt into symbols.cpp", file=sys.stderr)


if __name__ == "__main__":
    main()
