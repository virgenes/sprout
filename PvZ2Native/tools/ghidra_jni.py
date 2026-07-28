#!/usr/bin/env python3
"""ghidra_jni.py — Auto-detect JNINativeMethod tables from any libPVZ2.so.

Usage:
    python ghidra_jni.py path/to/libPVZ2.so [--name "X.Y.Z"] [--ghidra-dir C:/ghidra/ghidra_12.1.2_PUBLIC]

Output: a kVersions entry ready to paste into symbols.cpp, plus a JSON dump
of every native method found (including secondary arrays that JNI_OnLoad
registers indirectly).

Dependencies:
    pip install pyghidra jpype1
    (or install from Ghidra's PyGhidra/pypkg/dist/)
"""

import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path


NATIVE_ORDER = [
    ("game_app_initialize",             "nativeInitialize"),
    ("application_will_finish_launching","applicationWillFinishLaunching"),
    ("application_did_finish_launching", "applicationDidFinishLaunching"),
    ("application_will_become_foreground","applicationWillBecomeForeground"),
    ("application_did_become_active",    "applicationDidBecomeActive"),
    ("on_surface_created",              "onSurfaceCreated"),
    ("on_surface_changed",              "onSurfaceChanged"),
    ("on_draw_frame",                   "onDrawFrame"),
    ("pump_message_queue",              "PumpMessageQueue"),
]


def parse_args():
    parser = argparse.ArgumentParser(description="Ghidra JNINativeMethod extractor")
    parser.add_argument("libpvz2", type=Path, help="Path to libPVZ2.so")
    parser.add_argument("--name", default="X.Y.Z", help="Version label for the output")
    parser.add_argument("--ghidra-dir", default=None,
                        help="Ghidra install dir (auto-detected if not given)")
    parser.add_argument("--json", type=Path, default=None,
                        help="Also dump all methods as JSON to this file")
    parser.add_argument("--no-analysis", action="store_true",
                        help="Skip auto-analysis (load existing snapshot)")
    return parser.parse_args()


def open_ghidra(so_path: Path, ghidra_dir: str = None, analyze: bool = True):
    """Open a .so in Ghidra using PyGhidra's headless bridge."""
    import pyghidra

    kwargs = {"verbose": True}
    if ghidra_dir:
        kwargs["install_dir"] = ghidra_dir

    # Start Ghidra headless
    launcher = pyghidra.HeadlessPyGhidraLauncher(**kwargs)
    launcher.start()

    # Open the program
    ctx = launcher.open_program(
        str(so_path),
        analyze=analyze,
        project_name="pvz2_re",
        project_location=str(so_path.parent),
    )
    return ctx


def get_function_at(ctx, offset: int):
    """Get Ghidra function at a raw .so offset (before base)."""
    pgm = ctx.program
    addr = pgm.address_factory.getDefaultAddressSpace().getAddress(offset)
    return pgm.listing.getFunctionContaining(addr)


def find_jni_onload(ctx) -> int:
    """Locate JNI_OnLoad in the dynamic symbol table."""
    pgm = ctx.program
    sym_table = pgm.symbol_table
    for sym in sym_table.getSymbols("JNI_OnLoad"):
        return sym.address.offset
    return None


def extract_method_arrays(ctx, fn_addr: int, so_path: Path) -> list:
    """Find all JNINativeMethod arrays referenced from JNI_OnLoad.

    Uses capstone (fallback) + Ghidra's data references to find every
    RegisterNatives call and read the gMethods array at each call site.
    """
    # First try Ghidra's references
    pgm = ctx.program
    methods = []

    # Get the raw bytes for disassembly
    with open(so_path, "rb") as f:
        raw = bytearray(f.read())

    # Use capstone to disassemble JNI_OnLoad
    try:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
        from capstone.arm import ARM_OP_REG, ARM_OP_MEM, ARM_OP_IMM, ARM_REG_PC, ARM_REG_SP, ARM_REG_LR
    except ImportError:
        print("Warning: capstone not available, using Ghidra decompiler only")
        return find_methods_via_xrefs(ctx, fn_addr)

    # Try ARM mode first
    for mode, mode_name in [(CS_MODE_ARM, "ARM"), (CS_MODE_THUMB, "Thumb")]:
        dis = Cs(CS_ARCH_ARM, mode)
        dis.detail = True

        # Find the function bounds from Ghidra
        fn = get_function_at(ctx, fn_addr)
        if fn is None:
            continue
        fn_start = fn.body.minAddress.offset
        fn_end = fn.body.maxAddress.offset
        code = raw[fn_start:fn_end]

        regs = {}
        got_base = None

        for ins in dis.disasm(bytes(code), fn_start):
            if ins.mnemonic in ("bl", "blx"):
                target = None
                if len(ins.operands) >= 1:
                    op0 = ins.operands[0]
                    if op0.type == ARM_OP_IMM:
                        target = op0.imm
                    elif op0.type == ARM_OP_REG and op0.reg in regs:
                        target = regs[op0.reg]

                if target is None:
                    continue

                # Check if this is a RegisterNatives call by examining r0-r2
                r0 = regs.get(0)
                r1 = regs.get(1)
                r2 = regs.get(2)

                if r0 is not None and r1 is not None and r2 is not None:
                    # r1 = JNIEnv*, r2 = clazz, r2/r1 = gMethods array
                    # Actually RegisterNatives(JNIEnv*, jclass, JNINativeMethod*, int)
                    # r0=JNIEnv, r1=clazz, r2=array_ptr, r3=count
                    gmethods = r2
                    count = regs.get(3, 0)

                    if isinstance(gmethods, int) and gmethods < len(raw):
                        entries = read_method_table(raw, gmethods, so_path)
                        if entries:
                            methods.extend(entries)

        if methods:
            break

    # Also scan the whole binary for method arrays using Ghidra's data analysis
    ghidra_methods = find_methods_via_xrefs(ctx, fn_addr)
    methods.extend(ghidra_methods)

    return dedup_methods(methods)


def find_methods_via_xrefs(ctx, fn_addr: int) -> list:
    """Use Ghidra's data reference analysis to find method arrays."""
    pgm = ctx.program
    methods = []

    # Get the decompiler for JNI_OnLoad
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    iface = DecompInterface()
    iface.openProgram(pgm)

    fn = get_function_at(ctx, fn_addr)
    if fn is None:
        return methods

    results = iface.decompileFunction(fn, 0, ConsoleTaskMonitor())
    if results and results.decompileCompleted():
        high_func = results.highFunction
        # Iterate through all PCode ops looking for CALLI/CALL to RegisterNatives
        for op in high_func.getOps():
            if op.opcode == 13:  # CALL
                # Check operands for method array references
                pass

    # Fallback: scan for JNINativeMethod structures (12-byte triplets)
    with open(str(pgm.executablePath), "rb") as f:
        raw = bytearray(f.read())

    for off in range(0x100000, len(raw) - 12, 4):
        if is_method_array(raw, off):
            entries = read_method_table(raw, off, Path(pgm.executablePath))
            if entries:
                methods.extend(entries)

    return methods


def is_method_array(data: bytearray, offset: int) -> bool:
    """Check if offset looks like a JNINativeMethod array entry."""
    if offset + 12 > len(data):
        return False
    name_ptr = struct.unpack_from("<I", data, offset)[0]
    sig_ptr = struct.unpack_from("<I", data, offset + 4)[0]
    fn_ptr = struct.unpack_from("<I", data, offset + 8)[0]

    if name_ptr == 0 and sig_ptr == 0 and fn_ptr == 0:
        return False
    if not (0x1000000 < name_ptr < len(data)):
        return False
    if not (0x1000000 < sig_ptr < len(data)):
        return False
    if not (0x1000000 < fn_ptr < len(data)):
        return False

    # Read strings
    try:
        name_end = data.index(b"\x00", name_ptr)
        name = data[name_ptr:name_end].decode("ascii", errors="replace")
        sig_end = data.index(b"\x00", sig_ptr)
        sig = data[sig_ptr:sig_end].decode("ascii", errors="replace")
    except (ValueError, UnicodeDecodeError):
        return False

    if len(name) < 2 or len(name) > 80:
        return False
    if not sig.startswith("("):
        return False

    return True


def read_method_table(data: bytearray, array_offset: int, so_path: Path) -> list:
    """Read JNINativeMethod[count] starting at array_offset."""
    entries = []
    off = array_offset

    while off + 12 <= len(data):
        name_ptr, sig_ptr, fn_ptr = struct.unpack_from("<III", data, off)

        if name_ptr == 0 and sig_ptr == 0 and fn_ptr == 0:
            break
        if not (0x1000000 < name_ptr < len(data)):
            break
        if not (0x1000000 < sig_ptr < len(data)):
            break
        if not (0x1000000 < fn_ptr < len(data)):
            break

        try:
            name_end = data.index(b"\x00", name_ptr)
            name = data[name_ptr:name_end].decode("ascii", errors="replace")
            sig_end = data.index(b"\x00", sig_ptr)
            sig = data[sig_ptr:sig_end].decode("ascii", errors="replace")
        except (ValueError, UnicodeDecodeError):
            break

        if len(name) < 1 or len(name) > 100:
            break
        if not sig:
            break

        entries.append({
            "name": name,
            "signature": sig,
            "offset": fn_ptr,
            "array_offset": array_offset,
        })
        off += 12

    return entries


def dedup_methods(methods: list) -> list:
    """Remove duplicates, keeping the last occurrence (preferring fuller info)."""
    seen = {}
    for m in methods:
        key = (m["name"], m["signature"])
        seen[key] = m
    return list(seen.values())


def fingerprint(data: bytearray, offset: int) -> int:
    """Read 8 bytes at .so offset as little-endian u64."""
    if offset + 8 > len(data):
        return 0
    return struct.unpack_from("<Q", data, offset)[0]


def generate_symbols_entry(methods: list, version: str, raw: bytearray) -> str:
    """Generate the kVersions C++ code for symbols.cpp."""
    native_map = {}
    extra_methods = []

    for m in methods:
        matched = False
        for field_name, java_name in NATIVE_ORDER:
            if java_name.lower() in m["name"].lower() or m["name"].lower() in java_name.lower():
                native_map[field_name] = m["offset"]
                matched = True
                break
        if not matched:
            extra_methods.append(m)

    lines = []
    lines.append(f"    /* --- {version} -------------------------------------------------- */")
    lines.append("    {")
    lines.append(f'        "{version}",')

    # native block
    lines.append("        /* native */ {")
    for field_name, _ in NATIVE_ORDER:
        off = native_map.get(field_name, 0)
        tag = "  /* NOT FOUND */" if off == 0 else ""
        lines.append(f"            0x{off:08x}, /* {field_name:50s} */{tag}")
    lines.append("        },")

    # surface_changed_pad (heuristic: 2 for ARM, 1 for Thumb)
    lines.append("        /* surface_changed_pad */ 2,")

    # globals (to be filled by ghidra_globals.py)
    lines.append("        /* global */ {")
    lines.append("            0,         /* LawnApp -- locate me */")
    lines.append("            0,         /* AndroidAppDriver -- locate me */")
    lines.append("        },")

    # fn
    lines.append("        /* fn */ {0},")

    # jni_native (http_transaction_error)
    http_offsets = [m["offset"] for m in methods if "http" in m["name"].lower()
                    and ("error" in m["name"].lower() or "fail" in m["name"].lower())]
    http_val = http_offsets[0] if http_offsets else 0
    lines.append("        /* jni_native */ {")
    lines.append(f"            0x{http_val:08x}, /* http_transaction_error */")
    lines.append("        },")

    # input
    lines.append("        /* input */ {},")

    # fingerprints
    on_draw = native_map.get("on_draw_frame", 0)
    game_app_init = native_map.get("game_app_initialize", 0)
    fp_draw = fingerprint(raw, on_draw)
    fp_init = fingerprint(raw, game_app_init)

    lines.append(f"        0x{fp_draw:016x}ull, /* fingerprint onDrawFrame */")
    lines.append(f"        0x{fp_init:016x}ull, /* fingerprint GameAppInitialize */")
    lines.append("    },")

    lines.append("")
    lines.append("/* Extra methods found but not mapped to known fields:")
    for m in extra_methods:
        lines.append(f"   0x{m['offset']:08x}  {m['name']:40s}  {m['signature']}")
    lines.append("*/")

    return "\n".join(lines)


def main():
    args = parse_args()

    so_path = args.libpvz2
    if not so_path.exists():
        print(f"error: {so_path} not found", file=sys.stderr)
        sys.exit(1)

    with open(so_path, "rb") as f:
        raw = bytearray(f.read())

    print(f"Image: {len(raw)} bytes ({(len(raw) + 0xFFFFF) >> 20} MB)", file=sys.stderr)
    print(file=sys.stderr)

    # Open in Ghidra headless
    print("Starting Ghidra headless...", file=sys.stderr)
    ctx = open_ghidra(so_path, args.ghidra_dir, analyze=not args.no_analysis)

    pgm = ctx.program
    print(f"Loaded: {pgm.name} ({pgm.listing.numInstructions} instructions)", file=sys.stderr)
    print(file=sys.stderr)

    # Find JNI_OnLoad
    jni_off = find_jni_onload(ctx)
    if jni_off is None:
        print("JNI_OnLoad not found in dynamic symbols!", file=sys.stderr)
        # Try scanning sections
        for block in pgm.memory.blocks:
            if ".dynsym" in block.name:
                print(f"  Found {block.name} at {block.start}", file=sys.stderr)
        sys.exit(1)

    print(f"JNI_OnLoad at 0x{jni_off:08x}", file=sys.stderr)

    # Extract all method tables
    methods = extract_method_arrays(ctx, jni_off, so_path)

    if not methods:
        print("No methods found via RegisterNatives trace.", file=sys.stderr)
        print("Scanning entire binary for JNINativeMethod arrays...", file=sys.stderr)
        for off in range(0x100000, len(raw) - 12, 4):
            if is_method_array(raw, off):
                entries = read_method_table(raw, off, so_path)
                if len(entries) >= 3:
                    print(f"  Found array at 0x{off:08x}: {len(entries)} methods", file=sys.stderr)
                    methods.extend(entries)

    methods = dedup_methods(methods)

    if not methods:
        print("FATAL: no JNI methods found!", file=sys.stderr)
        ctx.close()
        sys.exit(1)

    print(f"\nFound {len(methods)} unique native methods:\n", file=sys.stderr)
    for i, m in enumerate(sorted(methods, key=lambda x: x["offset"])):
        print(f"  [{i:2d}] 0x{m['offset']:08x}  {m['name']:40s}  {m['signature']}", file=sys.stderr)

    # Generate symbols.cpp entry
    print(file=sys.stderr)
    print("=" * 70, file=sys.stderr)
    print(f"  kVersions entry for \"{args.name}\"", file=sys.stderr)
    print("=" * 70, file=sys.stderr)
    print(file=sys.stderr)

    entry = generate_symbols_entry(methods, args.name, raw)
    print(entry)

    # Also regenerate gen_symbols.py-style output
    print(file=sys.stderr)
    print("Raw fingerprints:", file=sys.stderr)
    on_draw = next((m["offset"] for m in methods if "draw" in m["name"].lower()), 0)
    gaminit = next((m["offset"] for m in methods if "initialize" in m["name"].lower() or "GameAppInitialize" in m["name"]), 0)
    print(f"  onDrawFrame:        0x{fingerprint(raw, on_draw):016x}", file=sys.stderr)
    print(f"  GameAppInitialize:  0x{fingerprint(raw, gaminit):016x}", file=sys.stderr)

    # JSON dump
    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "version": args.name,
                "image_size": len(raw),
                "jni_onload": jni_off,
                "methods": methods,
            }, f, indent=2)
        print(f"\nDumped {args.json}", file=sys.stderr)

    ctx.close()


if __name__ == "__main__":
    main()
