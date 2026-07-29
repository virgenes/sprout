#!/usr/bin/env python3
"""ghidra_globals.py — Find game object global pointers (LawnApp, AndroidAppDriver, etc.)

This script loads libPVZ2.so in Ghidra headless and locates critical global
variables by tracing operator new calls inside GameAppInitialize.

Strategy:
  1. Find GameAppInitialize from JNINativeMethod table
  2. Decompile it and look for calls to operator new (Znwj / Znam)
  3. Track the return value (r0) forward to STR instructions that store it
     to a BSS address
  4. Cross-reference known usages of each BSS address to identify which
     pointer is LawnApp, AndroidAppDriver, etc.

Usage:
    python ghidra_globals.py path/to/libPVZ2.so [--ghidra-dir DIR]
"""

import argparse
import json
import struct
import sys
from pathlib import Path


BSS_PATTERNS = {
    "LawnApp": {
        "description": "Main game application object (SexyAppFramework)",
        "size_hint": (0x100, 0x1000),
        "xref_hints": ["onDrawFrame", "OnCanvasButton", "advance"],
    },
    "AndroidAppDriver": {
        "description": "Android driver (surface/frame natives dereference this)",
        "size_hint": (0x10, 0x200),
        "xref_hints": ["onSurfaceCreated", "onSurfaceChanged"],
    },
    "LawnMower": {
        "description": "Mower object array/instance",
        "size_hint": (0x10, 0x100),
        "xref_hints": [],
    },
    "StoreScreen": {
        "description": "In-app purchase store screen",
        "size_hint": (0x100, 0x1000),
        "xref_hints": [],
    },
}


def parse_args():
    parser = argparse.ArgumentParser(description="Ghidra global variable finder")
    parser.add_argument("libpvz2", type=Path, help="Path to libPVZ2.so")
    parser.add_argument("--ghidra-dir", default=None)
    parser.add_argument("--json", type=Path, default=None, help="Output JSON")
    parser.add_argument("--method-json", type=Path, default=None,
                        help="Path to ghidra_jni.py JSON output (pre-extracted methods)")
    parser.add_argument("--no-analysis", action="store_true")
    return parser.parse_args()


def open_ghidra(so_path, ghidra_dir=None, analyze=True):
    import os
    import pyghidra
    from pyghidra import open_program

    if ghidra_dir:
        os.environ["GHIDRA_INSTALL_DIR"] = str(ghidra_dir)

    ctx_wrapper = open_program(
        str(so_path), analyze=analyze,
        project_name="pvz2_re", project_location=str(so_path.parent),
    )
    api = ctx_wrapper.__enter__()
    return _GhidraContext(ctx_wrapper, api)


class _GhidraContext:
    """Wrapper around FlatProgramAPI with proper cleanup."""

    def __init__(self, ctx_mgr, api):
        self._ctx_mgr = ctx_mgr
        self.api = api
        self.program = api.currentProgram if api else None

    def close(self):
        if self._ctx_mgr:
            self._ctx_mgr.__exit__(None, None, None)
            self._ctx_mgr = None


def read_jni_methods(so_path):
    """If --method-json wasn't given, scan the binary directly."""
    with open(so_path, "rb") as f:
        data = bytearray(f.read())

    methods = []
    for off in range(0x100000, len(data) - 12, 4):
        name_ptr, sig_ptr, fn_ptr = struct.unpack_from("<III", data, off)
        if not (0x1000000 < name_ptr < len(data)):
            continue
        if not (0x1000000 < sig_ptr < len(data)):
            continue
        if name_ptr == 0 and sig_ptr == 0:
            continue
        try:
            n_end = data.index(b"\x00", name_ptr)
            name = data[name_ptr:n_end].decode("ascii", errors="replace")
            s_end = data.index(b"\x00", sig_ptr)
            sig = data[sig_ptr:s_end].decode("ascii", errors="replace")
        except (ValueError, UnicodeDecodeError):
            continue
        if len(name) < 2 or len(name) > 80:
            continue
        if not sig.startswith("("):
            continue
        methods.append({"name": name, "signature": sig, "offset": fn_ptr})

    # Dedup
    seen, unique = set(), []
    for m in methods:
        key = (m["name"], m["signature"])
        if key not in seen:
            seen.add(key)
            unique.append(m)
    return unique


def find_game_app_initialize(methods):
    for m in methods:
        if "GameAppInitialize" in m["name"] or "nativeInitialize" in m["name"]:
            return m["offset"]
    return None


def trace_operator_new(data: bytearray, fn_start: int, fn_size: int, so_base: int = 0):
    """Trace operator new calls in a function and find STRs to BSS.

    Uses capstone to disassemble ARM code, tracking register values
    through MOV, LDR, ADD operations to identify calls to operator new
    and WHERE the returned pointer gets stored (STR to BSS).
    """
    try:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
        from capstone.arm import ARM_OP_REG, ARM_OP_MEM, ARM_OP_IMM, ARM_REG_PC
    except ImportError:
        print("ERROR: capstone required for operator new tracing")
        print("  pip install capstone")
        sys.exit(1)

    # Determine BSS range from ELF headers
    bss_lo, bss_hi = find_bss_range(data)

    code = data[fn_start:fn_start + fn_size]
    findings = []

    for mode, mode_name in [(CS_MODE_ARM, "ARM")]:
        dis = Cs(CS_ARCH_ARM, mode)
        dis.detail = True

        regs = {}
        call_sites = []
        str_to_bss = []

        for ins in dis.disasm(bytes(code), fn_start):
            ops = ins.operands
            addr = ins.address
            mnem = ins.mnemonic

            # MOV Rd, #imm
            if mnem == "mov" and len(ops) == 2 and ops[1].type == ARM_OP_IMM:
                regs[ops[0].reg] = ops[1].imm

            # LDR Rd, [PC, #imm] (literal pool load)
            if mnem == "ldr" and len(ops) >= 2:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC:
                    pool = ((addr + 8) & ~3) + (ops[1].mem.disp or 0)
                    if pool < len(data):
                        regs[rd] = struct.unpack_from("<I", data, pool)[0]

            # LDR Rd, [Rn, #imm] (GOT dereference)
            if mnem == "ldr" and len(ops) >= 2:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_MEM:
                    base = ops[1].mem.base
                    disp = ops[1].mem.disp or 0
                    if base in regs:
                        load_addr = regs[base] + disp
                        if load_addr < len(data):
                            regs[rd] = struct.unpack_from("<I", data, load_addr)[0]

            # ADD Rd, PC, Rm (GOT base computation)
            if mnem == "add" and len(ops) == 3:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_REG:
                    base_r = ops[1].reg
                    off_r = ops[2].reg
                    if base_r == ARM_REG_PC and off_r in regs and isinstance(regs[off_r], int):
                        regs[rd] = addr + 8 + regs[off_r]
                # ADD Rd, Rn, #imm
                if ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
                    if ops[1].reg in regs and isinstance(regs[ops[1].reg], int):
                        regs[rd] = regs[ops[1].reg] + ops[2].imm

            # Detect calls — especially operator new
            if mnem in ("bl", "blx"):
                target = None
                if ops[0].type == ARM_OP_IMM:
                    target = ops[0].imm
                elif ops[0].type == ARM_OP_REG and ops[0].reg in regs:
                    target = regs[ops[0].reg]

                if target is not None:
                    r0_size = regs.get(0, -1)
                    call_sites.append({
                        "addr": addr,
                        "target": target,
                        "r0": r0_size,
                        "regs_at_call": dict(regs),
                    })
                    # operator new returns in r0 — clear it (unknown after call)
                    regs.pop(0, None)

            # STR Rd, [Rn, #imm] — detect stores to BSS
            if mnem.startswith("str") and len(ops) >= 2:
                rd = ops[0]
                if ops[1].type == ARM_OP_MEM:
                    base = ops[1].mem.base
                    disp = ops[1].mem.disp or 0
                    if base in regs and isinstance(regs[base], int):
                        target = regs[base] + disp
                        if bss_lo <= target <= bss_hi:
                            src_reg = rd.reg if rd.type == ARM_OP_REG else -1
                            src_val = regs.get(src_reg) if src_reg >= 0 else None
                            str_to_bss.append({
                                "addr": addr,
                                "bss_addr": target,
                                "src_reg": src_reg,
                                "src_val": src_val,
                            })

        # Analyze call sites to find operator new calls
        findings = analyze_calls(call_sites, str_to_bss, data)
        if findings:
            break

    return findings


def find_bss_range(data: bytearray) -> tuple:
    """Extract BSS range from ELF program headers."""
    if data[:4] != b"\x7fELF":
        return 0x10000000, 0x20000000  # fallback

    is_arm = data[4] == 1  # 32-bit
    e_phoff = struct.unpack_from("<I" if is_arm else "<Q", data, 0x1C)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x2A if is_arm else 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C if is_arm else 0x38)[0]

    bss_lo, bss_hi = None, None
    for i in range(e_phnum):
        phdr = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, phdr)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_vaddr = struct.unpack_from("<I", data, phdr + 0x08)[0]
        p_filesz = struct.unpack_from("<I", data, phdr + 0x10)[0]
        p_memsz = struct.unpack_from("<I", data, phdr + 0x14)[0]
        if p_filesz < p_memsz:
            # Contains BSS (memory size > file size)
            bss_off = p_vaddr + p_filesz
            bss_end = p_vaddr + p_memsz
            if bss_lo is None or bss_off < bss_lo:
                bss_lo = bss_off
            if bss_hi is None or bss_end > bss_hi:
                bss_hi = bss_end

    if bss_lo is None:
        return 0x10000000, 0x20000000
    return bss_lo, bss_hi


def analyze_calls(calls: list, stores: list, data: bytearray) -> list:
    """Identify operator new calls and match them to BSS stores."""
    # Find operator new candidates: calls where r0 (size argument) is large
    new_calls = []
    for c in calls:
        if isinstance(c["r0"], int) and c["r0"] > 0x50:
            new_calls.append(c)

    # Match each new call to the next STR to BSS of the return value
    results = []
    for nc in new_calls:
        # Find the next STR to BSS after this call that stores a register
        # that was set by this call (the return value in r0)
        best_store = None
        for s in stores:
            if s["addr"] > nc["addr"]:
                best_store = s
                break

        if best_store:
            results.append({
                "call_addr": nc["addr"],
                "allocation_size": nc["r0"],
                "bss_addr": best_store["bss_addr"],
                "store_addr": best_store["addr"],
            })

    return results


def identify_globals(results: list, methods: list, data: bytearray) -> dict:
    """Cross-reference BSS addresses to identify known globals."""
    globals_found = {}

    # Known offsets from existing symbols.cpp
    known_1_6 = {"LawnApp": 0xD55650, "AndroidAppDriver": 0xDC8FD4}
    known_4_5 = {"LawnApp": 0x117A6F0, "AndroidAppDriver": 0x117A734}

    for r in results:
        bss = r["bss_addr"]
        size = r["allocation_size"]

        label = None
        confidence = 0

        # Check known offsets
        for name, known_addr in list(known_1_6.items()) + list(known_4_5.items()):
            if abs(bss - known_addr) < 8:
                label = name
                confidence = 10
                break

        # Heuristic: large allocation (0x100+) near other BSS = LawnApp
        if label is None and size > 0x100:
            for r2 in results:
                if r2 is r and abs(r2["bss_addr"] - bss) < 0x1000 and r2["allocation_size"] < 0x100:
                    # Found a small allocation near our large one
                    pass
            if 0x100000 < bss < 0x2000000:
                label = "suspected_LawnApp_or_major_object"
                confidence = 5

        # Heuristic: small allocation (0x10-0x100) = AndroidAppDriver
        if label is None and 0x10 <= size <= 0x200:
            label = "suspected_small_driver_or_manager"
            confidence = 3

        globals_found[hex(bss)] = {
            "bss_addr": bss,
            "allocation_size": size,
            "identified_as": label,
            "confidence": confidence,
            "call_offset": r["call_addr"],
            "store_offset": r["store_addr"],
        }

    return globals_found


def main():
    args = parse_args()
    so_path = args.libpvz2

    with open(so_path, "rb") as f:
        data = bytearray(f.read())

    print(f"Image: {len(data)} bytes ({(len(data) + 0xFFFFF) >> 20} MB)", file=sys.stderr)

    # Get JNI methods
    if args.method_json:
        with open(args.method_json) as f:
            info = json.load(f)
            methods = info["methods"]
    else:
        methods = read_jni_methods(so_path)

    game_app_init = find_game_app_initialize(methods)
    if game_app_init is None:
        print("ERROR: Could not find GameAppInitialize offset", file=sys.stderr)
        sys.exit(1)

    print(f"GameAppInitialize at 0x{game_app_init:08x}", file=sys.stderr)

    # Open in Ghidra for size estimation
    ctx = open_ghidra(so_path, args.ghidra_dir, analyze=not args.no_analysis)
    pgm = ctx.program

    fn = None
    if pgm:
        for f in pgm.listing.functions(None):
            if f.name and "GameAppInitialize" in f.name:
                fn = f
                break

    fn_size = 0x600
    if fn:
        fn_size = fn.body.maxAddress.offset - fn.body.minAddress.offset + 1
        print(f"Function size: {fn_size} bytes (from Ghidra)", file=sys.stderr)

    # Trace operator new
    findings = trace_operator_new(data, game_app_init, fn_size)
    globals_found = identify_globals(findings, methods, data)

    print(f"\nFound {len(findings)} potential new+store chains:\n", file=sys.stderr)
    for f_ in findings:
        label = globals_found.get(hex(f_["bss_addr"]), {}).get("identified_as", "unknown")
        print(f"  Call 0x{f_['call_addr']:08x}: new({f_['allocation_size']})", file=sys.stderr)
        print(f"    Store at 0x{f_['store_addr']:08x} → BSS 0x{f_['bss_addr']:08x}", file=sys.stderr)
        if label:
            print(f"    → {label}", file=sys.stderr)
        print(file=sys.stderr)

    # Also open in Ghidra and use decompiler to trace
    print("\n=== Ghidra Decompiler Analysis ===", file=sys.stderr)
    print(file=sys.stderr)

    if pgm:
        from ghidra.app.decompiler import DecompInterface
        from ghidra.util.task import ConsoleTaskMonitor

        iface = DecompInterface()
        iface.openProgram(pgm)

        if fn:
            results = iface.decompileFunction(fn, 0, ConsoleTaskMonitor())
            if results and results.decompileCompleted():
                decompiled = results.getDecompiledFunction().getC()
                print("Decompiled GameAppInitialize (first 50 lines):", file=sys.stderr)
                for line in decompiled.split("\n")[:50]:
                    print(f"  {line}", file=sys.stderr)

    ctx.close()

    # Output globals for symbols.cpp
    print("\n" + "=" * 70)
    print("  GLOBAL VARIABLE ENTRIES FOR symbols.cpp")
    print("=" * 70)
    print()

    # Find best candidates
    lawnapp = next(
        (g for g in globals_found.values() if "LawnApp" in (g.get("identified_as") or "")),
        None
    )
    appdriver = next(
        (g for g in globals_found.values() if "driver" in (g.get("identified_as") or "").lower()),
        None
    )

    if not lawnapp:
        # Pick the largest allocation
        candidates = sorted(globals_found.values(), key=lambda x: x["allocation_size"], reverse=True)
        if candidates:
            lawnapp = candidates[0]

    if not appdriver and len(globals_found) >= 2:
        candidates = sorted(globals_found.values(), key=lambda x: x["allocation_size"])
        for c in candidates:
            if c is not lawnapp:
                appdriver = c
                break

    lawnapp_addr = lawnapp['bss_addr'] if lawnapp else 0
    appdriver_addr = appdriver['bss_addr'] if appdriver else 0
    lawnapp_desc = f" /* LawnApp (size={lawnapp['allocation_size']}) */" if lawnapp else ""
    appdriver_desc = " /* AndroidAppDriver */" if appdriver else ""

    print("        /* global */ {")
    print(f"            0x{lawnapp_addr:08x},{lawnapp_desc}")
    print(f"            0x{appdriver_addr:08x},{appdriver_desc}")
    print("        },")

    for g in globals_found.values():
        if g is not lawnapp and g is not appdriver:
            print(f"  /* 0x{g['bss_addr']:08x}: new({g['allocation_size']}) at 0x{g['call_offset']:08x}, "
                  f"stored at 0x{g['store_addr']:08x} */")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "game_app_initialize": game_app_init,
                "function_size": fn_size,
                "findings": findings,
                "globals": {k: v for k, v in globals_found.items()},
            }, f, indent=2)
        print(f"\nWrote {args.json}", file=sys.stderr)


if __name__ == "__main__":
    main()
