#!/usr/bin/env python3
"""auto_re.py — Master PvZ2 Reverse Engineering Orchestrator.

Automates the entire RE pipeline:
  Phase 1 — Ghidra binary analysis:
    1a. ghidra_jni.py      — Extract JNINativeMethod tables (→ symbols.cpp entry)
    1b. ghidra_globals.py   — Find global pointers (LawnApp, AndroidAppDriver, etc.)
    1c. ghidra_vtables.py   — Scan vtable hierarchy

  Phase 2 — Game data extraction:
    2a. rsb_unpack.py       — Extract individual files from OBB (RSB/pgsr)
    2b. rton_parser.py       — Parse all RTON config files to JSON

  Phase 3 — Report generation:
    3.  Generate a comprehensive RE report

Usage:
    # Full pipeline (need libPVZ2.so + main.obb):
    python auto_re.py --lib path/to/libPVZ2.so --obb path/to/main.obb

    # Phase 1 only (Ghidra):
    python auto_re.py --lib path/to/libPVZ2.so --phase 1

    # Phase 2 only (Data extraction):
    python auto_re.py --obb path/to/main.obb --phase 2

    # Specify Ghidra install dir:
    python auto_re.py --lib libPVZ2.so --ghidra-dir C:/ghidra/ghidra_12.1.2_PUBLIC
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


SCRIPTS_DIR = Path(__file__).parent
DEFAULT_GHIDRA = r"C:\ghidra\ghidra_12.1.2_PUBLIC"


def parse_args():
    parser = argparse.ArgumentParser(
        description="PvZ2 Reverse Engineering Master Orchestrator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--lib", type=Path, help="Path to libPVZ2.so")
    parser.add_argument("--obb", type=Path, help="Path to main.obb (expansion pack)")
    parser.add_argument("--phase", type=int, choices=[1, 2, 3],
                        help="Run only a specific phase")
    parser.add_argument("--ghidra-dir", type=Path, default=None,
                        help=f"Ghidra install dir (default: {DEFAULT_GHIDRA})")
    parser.add_argument("--out", type=Path, default=Path("re_out"),
                        help="Output directory (default: re_out/)")
    parser.add_argument("--name", default=None,
                        help="Version name override")
    parser.add_argument("--skip-analysis", action="store_true",
                        help="Skip Ghidra auto-analysis (use cached)")
    parser.add_argument("--clean", action="store_true",
                        help="Clean output directory before starting")
    return parser.parse_args()


def log(msg: str):
    print(f"[{datetime.now():%H:%M:%S}] {msg}", file=sys.stderr)


def run_script(script: str, args_list: list, cwd: Path = None) -> bool:
    script_path = SCRIPTS_DIR / script
    if not script_path.exists():
        log(f"ERROR: {script} not found at {script_path}")
        return False

    cmd = [sys.executable, str(script_path)] + args_list
    log(f"Running: {Path(script).stem} {' '.join(str(a) for a in args_list[:4])}...")

    start = time.time()
    result = subprocess.run(
        cmd, cwd=cwd or SCRIPTS_DIR,
        capture_output=False,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )
    elapsed = time.time() - start

    if result.returncode == 0:
        log(f"  ✓ completed in {elapsed:.1f}s")
        return True
    else:
        log(f"  ✗ FAILED (rc={result.returncode}) in {elapsed:.1f}s")
        return False


def phase1_ghidra(args) -> dict:
    """Phase 1: Ghidra binary analysis."""
    out = args.out / "ghidra"
    out.mkdir(parents=True, exist_ok=True)

    if not args.lib or not args.lib.exists():
        log("ERROR: --lib required for Phase 1")
        return {"status": "skipped", "reason": "no lib"}

    so_path = args.lib.resolve()
    version = args.name or "X.Y.Z"
    ghidra_arg = ["--ghidra-dir", str(args.ghidra_dir)] if args.ghidra_dir else []
    no_analysis = ["--no-analysis"] if args.skip_analysis else []

    log(f"Phase 1: Ghidra Analysis of {so_path}")
    log(f"  Version: {version}")

    results = {"version": version, "library": str(so_path)}

    # Step 1a: JNI methods
    methods_json = out / "methods.json"
    symbols_txt = out / "methods_symbols.txt"

    ok = run_script("ghidra_jni.py", [
        str(so_path), "--name", version,
        "--json", str(methods_json),
    ] + ghidra_arg + no_analysis)

    if not ok:
        log("FATAL: JNI extraction failed")
        return {"status": "failed", "phase": "1a"}

    results["jni_methods"] = str(methods_json)
    results["jni_count"] = count_json_array(methods_json, "methods")

    # Save symbols output
    log(f"  JNI output: {methods_json}")

    # Step 1b: Globals
    globals_json = out / "globals.json"

    ok = run_script("ghidra_globals.py", [
        str(so_path), "--method-json", str(methods_json),
        "--json", str(globals_json),
    ] + ghidra_arg + no_analysis)

    if ok:
        results["globals"] = str(globals_json)

    # Step 1c: Vtables
    vtables_json = out / "vtables.json"

    ok = run_script("ghidra_vtables.py", [
        str(so_path), "--json", str(vtables_json),
        "--min-vtable", "5",
    ] + ghidra_arg + no_analysis)

    if ok:
        results["vtables"] = str(vtables_json)
        results["vtable_count"] = count_json_key(vtables_json, "total_vtables")

    results["status"] = "completed"
    log("Phase 1 complete.")
    return results


def phase2_data(args) -> dict:
    """Phase 2: Game data extraction (OBB → RSB → pgsr → RTON)."""
    out = args.out / "data"
    out.mkdir(parents=True, exist_ok=True)

    if not args.obb or not args.obb.exists():
        log("ERROR: --obb required for Phase 2")
        return {"status": "skipped", "reason": "no obb"}

    obb_path = args.obb.resolve()
    log(f"Phase 2: Data Extraction from {obb_path}")

    results = {"obb": str(obb_path)}

    # Step 2a: Extract RSB → pgsr → individual files
    log("Step 2a: Extracting OBB files...")
    obb_out = out / "extracted"
    obb_listing = out / "obb_listing.json"

    ok = run_script("rsb_unpack.py", [
        str(obb_path), str(obb_out),
        "--json", str(obb_listing),
    ])

    if not ok:
        log("WARN: OBB extraction failed or incomplete")

    # Count extracted files
    file_count = 0
    rton_count = 0
    if obb_out.exists():
        for f in obb_out.rglob("*"):
            if f.is_file():
                file_count += 1
                if f.suffix.upper() in (".RTON", ".JSON"):
                    rton_count += 1

    results["extracted_files"] = file_count
    results["rton_files"] = rton_count
    log(f"  Extracted {file_count} files ({rton_count} RTON configs)")

    # Step 2b: Parse all RTON files to JSON
    log("Step 2b: Parsing RTON configuration files...")
    rton_out = out / "rton_json"

    if rton_count > 0:
        ok = run_script("rton_parser.py", [
            str(obb_out), str(rton_out), "--extract-all", "--pretty",
        ])

        if ok:
            json_count = sum(1 for _ in rton_out.rglob("*.json"))
            results["parsed_rton"] = json_count
            log(f"  Parsed {json_count} RTON files to JSON")
    else:
        log("  No RTON files found to parse")

    results["status"] = "completed"
    log("Phase 2 complete.")
    return results


def phase3_report(args, results: dict) -> None:
    """Phase 3: Generate comprehensive RE report."""
    out = args.out
    report_path = out / "REPORT.md"

    lines = []
    lines.append("# PvZ2 Reverse Engineering Report")
    lines.append(f"Generated: {datetime.now():%Y-%m-%d %H:%M:%S}")
    lines.append("")

    # Phase 1 results
    if "version" in results:
        lines.append("## Phase 1: Ghidra Binary Analysis")
        lines.append(f"- **Library:** {results.get('library', 'N/A')}")
        lines.append(f"- **Version:** {results.get('version', 'Unknown')}")
        lines.append(f"- **JNI Methods:** {results.get('jni_count', '?')} found")
        lines.append(f"- **Vtables:** {results.get('vtable_count', '?')} found")

        if "jni_methods" in results:
            lines.append(f"- **Methods JSON:** `{results['jni_methods']}`")
        if "globals" in results:
            lines.append(f"- **Globals:** `{results['globals']}`")
        if "vtables" in results:
            lines.append(f"- **Vtables JSON:** `{results['vtables']}`")
        lines.append("")

    # Phase 2 results
    if "obb" in results:
        lines.append("## Phase 2: Game Data Extraction")
        lines.append(f"- **OBB:** {results.get('obb', 'N/A')}")
        lines.append(f"- **Extracted files:** {results.get('extracted_files', 0)}")
        lines.append(f"- **RTON configs:** {results.get('rton_files', 0)}")
        lines.append(f"- **Parsed RTON to JSON:** {results.get('parsed_rton', 0)}")
        lines.append("")

    # Output structure
    lines.append("## Output Structure")
    lines.append("```")
    for f in sorted(out.rglob("*")):
        if f.is_dir():
            lines.append(f"{f.relative_to(out)}/")
    lines.append("```")
    lines.append("")

    # Next steps
    lines.append("## Next Steps")
    lines.append("1. **Copy ghidra/methods_symbols.txt** into `src/game/symbols.cpp`")
    lines.append("2. **Review globals** in `ghidra/globals.json` to identify game objects")
    lines.append("3. **Explore RTON configs** in `data/rton_json/` for game parameters:")
    lines.append("   - Plant stats (damage, HP, cost, cooldown)")
    lines.append("   - Zombie stats (HP, speed, damage)")
    lines.append("   - Level definitions (waves, zombie types)")
    lines.append("   - World maps and progression")
    lines.append("4. **Modify and reinject** changed RTON files back into the OBB")
    lines.append("")

    report_path.write_text("\n".join(lines))
    log(f"Report generated: {report_path}")


def count_json_array(path: Path, key: str) -> int:
    """Count elements in a JSON array."""
    try:
        with open(path) as f:
            data = json.load(f)
            return len(data.get(key, []))
    except Exception:
        return 0


def count_json_key(path: Path, key: str) -> int:
    """Read an integer value from a JSON key."""
    try:
        with open(path) as f:
            data = json.load(f)
            return data.get(key, 0)
    except Exception:
        return 0


def main():
    args = parse_args()

    # Auto-detect Ghidra dir
    if args.ghidra_dir is None:
        candidate = Path(DEFAULT_GHIDRA)
        if candidate.exists():
            args.ghidra_dir = candidate
            log(f"Auto-detected Ghidra at {candidate}")

    # Clean
    if args.clean and args.out.exists():
        log(f"Cleaning {args.out}")
        shutil.rmtree(args.out)

    args.out.mkdir(parents=True, exist_ok=True)

    # Run phases
    all_results = {}
    phase = args.phase

    if phase is None or phase == 1:
        r1 = phase1_ghidra(args)
        all_results.update(r1)

    if phase is None or phase == 2:
        r2 = phase2_data(args)
        all_results.update(r2)

    # Generate report
    if phase is None or phase == 3:
        phase3_report(args, all_results)

    # Summary
    log("")
    log("=" * 60)
    log("  AUTO-RE COMPLETE")
    log("=" * 60)
    log(f"  Output directory: {args.out.resolve()}")
    log(f"  Report: {args.out / 'REPORT.md'}")
    log("")


if __name__ == "__main__":
    main()
