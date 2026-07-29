#!/usr/bin/env python3
"""ghidra_jni.py — Extract JNINativeMethod tables from libPVZ2.so.

Two modes:
  1. Fast mode (default): capstone disassembly of raw binary — no Ghidra needed.
  2. Ghidra mode (--ghidra-dir): uses Ghidra decompiler for verification.

Usage:
    python ghidra_jni.py path/to/libPVZ2.so [--name "X.Y.Z"]
    python ghidra_jni.py path/to/libPVZ2.so --ghidra-dir C:/ghidra/ghidra_12.1.2_PUBLIC
"""

import argparse
import json
import os
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
    parser = argparse.ArgumentParser(description="JNINativeMethod extractor")
    parser.add_argument("libpvz2", type=Path, help="Path to libPVZ2.so")
    parser.add_argument("--name", default="X.Y.Z", help="Version label")
    parser.add_argument("--ghidra-dir", default=None, help="Ghidra install dir (optional)")
    parser.add_argument("--json", type=Path, default=None, help="Output JSON")
    return parser.parse_args()


# ─── Raw binary analysis (no Ghidra) ───────────────────────────────────────

def r32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def find_jni_onload_raw(data):
    """Find JNI_OnLoad via ELF .dynsym."""
    if data[:4] != b"\x7fELF":
        return None
    is_arm = data[4] == 1  # 32-bit
    e_shoff = r32(data, 0x20)
    e_shentsize = struct.unpack_from("<H", data, 0x2E if is_arm else 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x30 if is_arm else 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x32 if is_arm else 0x3E)[0]
    if not e_shoff or not e_shnum:
        return None

    shdr_str = e_shoff + e_shstrndx * e_shentsize
    strtab_off = r32(data, shdr_str + 0x10)
    strtab_sz = r32(data, shdr_str + 0x14)
    strtab = data[strtab_off:strtab_off + strtab_sz]

    shdr_size = e_shentsize
    dynsym_off = dynstr_off = None
    for i in range(e_shnum):
        shdr = e_shoff + i * shdr_size
        sh_name = r32(data, shdr)
        sh_type = r32(data, shdr + 0x04)
        name_end = strtab.index(b"\0", sh_name) if b"\0" in strtab[sh_name:] else len(strtab)
        name = strtab[sh_name:name_end].decode("ascii", errors="replace") if sh_name < len(strtab) else ""
        if name == ".dynsym":
            dynsym_off = r32(data, shdr + 0x10)
            dynsym_sz = r32(data, shdr + 0x14)
        elif name == ".dynstr":
            dynstr_off = r32(data, shdr + 0x10)
            dynstr_sz = r32(data, shdr + 0x14)

    if not dynsym_off or not dynstr_off:
        return None

    for i in range(dynsym_sz // 0x10):
        sym = dynsym_off + i * 0x10
        st_name = r32(data, sym)
        st_info = data[sym + 0x0C]
        st_value = r32(data, sym + 0x04)
        if st_name > 0 and st_name < dynstr_sz:
            sym_name = data[dynstr_off + st_name:dynstr_off + dynstr_sz].split(b"\0")[0].decode("ascii", errors="replace")
            if sym_name == "JNI_OnLoad":
                return st_value
    return None


def trace_register_natives(data, fn_addr, fn_size=0x800):
    """Disassemble a function to find RegisterNatives calls and extract method arrays."""
    try:
        from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
        from capstone.arm import ARM_OP_REG, ARM_OP_MEM, ARM_OP_IMM, ARM_REG_PC
    except ImportError:
        print("ERROR: capstone required. Install with: pip install capstone", file=sys.stderr)
        sys.exit(1)

    methods = []
    seen_arrays = set()

    for mode, mode_name in [(CS_MODE_ARM, "ARM")]:
        md = Cs(CS_ARCH_ARM, mode)
        md.detail = True

        code = data[fn_addr:fn_addr + fn_size]
        regs = {}

        for ins in md.disasm(bytes(code), fn_addr):
            ops = ins.operands
            mnem = ins.mnemonic

            # Track MOV Rd, #imm
            if mnem == "mov" and len(ops) == 2 and ops[1].type == ARM_OP_IMM:
                regs[ops[0].reg] = ops[1].imm

            # Track LDR Rd, [PC, #imm]
            if mnem == "ldr" and len(ops) >= 2:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC:
                    pool = ((ins.address + 8) & ~3) + (ops[1].mem.disp or 0)
                    if pool < len(data):
                        regs[rd] = r32(data, pool)

            # Track ADD Rd, PC, Rm (GOT base)
            if mnem == "add" and len(ops) == 3:
                if ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_REG:
                    if ops[1].reg == ARM_REG_PC and ops[2].reg in regs:
                        regs[ops[0].reg] = ins.address + 8 + regs[ops[2].reg]

            # Track LDR Rd, [Rn, #imm] (dereference)
            if mnem == "ldr" and len(ops) >= 2:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_MEM:
                    base = ops[1].mem.base
                    disp = ops[1].mem.disp or 0
                    if base in regs and isinstance(regs[base], int) and 0 < regs[base] + disp < len(data):
                        regs[rd] = r32(data, regs[base] + disp)

            # Detect calls
            if mnem in ("bl", "blx"):
                target = None
                if ops[0].type == ARM_OP_IMM:
                    target = ops[0].imm
                elif ops[0].type == ARM_OP_REG and ops[0].reg in regs:
                    target = regs[ops[0].reg]

                if target is not None:
                    r2 = regs.get(2)
                    if isinstance(r2, int) and r2 > 0x100000:
                        # r2 likely holds the gMethods array
                        if r2 not in seen_arrays and r2 + 12 < len(data):
                            entries = read_jni_table(data, r2)
                            if entries:
                                methods.extend(entries)
                                seen_arrays.add(r2)

                    regs.pop(0, None)  # return value unknown

    return methods


def read_jni_table(data, array_off):
    """Read a JNINativeMethod[count] table from raw data."""
    entries = []
    off = array_off
    while off + 12 <= len(data):
        name_ptr, sig_ptr, fn_ptr = struct.unpack_from("<III", data, off)
        if name_ptr == 0 and sig_ptr == 0 and fn_ptr == 0:
            break
        if not (0x1000000 < name_ptr < len(data)):
            break
        if not (0x1000000 < sig_ptr < len(data)):
            break
        try:
            n_end = data.index(b"\x00", name_ptr)
            name = data[name_ptr:n_end].decode("ascii", errors="replace")
            s_end = data.index(b"\x00", sig_ptr)
            sig = data[sig_ptr:s_end].decode("ascii", errors="replace")
        except (ValueError, UnicodeDecodeError):
            break
        if len(name) < 2 or len(name) > 100:
            break
        entries.append({"name": name, "signature": sig, "offset": fn_ptr, "array_offset": array_off})
        off += 12
    return entries


def scan_all_method_arrays(data):
    """Scan entire binary for JNINativeMethod tables."""
    methods = []
    seen = set()
    for off in range(0x100000, len(data) - 12, 4):
        name_ptr, sig_ptr, fn_ptr = struct.unpack_from("<III", data, off)
        if name_ptr == 0 and sig_ptr == 0:
            continue
        if not (0x1000000 < name_ptr < len(data)) or not (0x1000000 < sig_ptr < len(data)):
            continue
        try:
            n_end = data.index(b"\x00", name_ptr)
            name = data[name_ptr:n_end].decode("ascii", errors="replace")
            s_end = data.index(b"\x00", sig_ptr)
            sig = data[sig_ptr:s_end].decode("ascii", errors="replace")
        except Exception:
            continue
        if len(name) < 2 or len(name) > 80 or not sig.startswith("("):
            continue
        if name in seen:
            continue
        seen.add(name)
        methods.append({"name": name, "signature": sig, "offset": fn_ptr, "array_offset": off})
    return methods


def fingerprint(data, offset):
    if offset + 8 > len(data):
        return 0
    return struct.unpack_from("<Q", data, offset)[0]


def generate_symbols_entry(methods, version, data):
    native_map = {}
    extra = []

    for m in methods:
        matched = False
        mn_lower = m["name"].lower()
        for field_name, java_name in NATIVE_ORDER:
            jn_lower = java_name.lower()
            # Try multiple match strategies:
            # 1. Substring match either way
            if jn_lower in mn_lower or mn_lower in jn_lower:
                native_map[field_name] = m["offset"]
                matched = True
                break
            # 2. Normalised: strip "Native_" prefix, check field name keywords
            clean = mn_lower.replace("native_", "").replace("_", "").replace(" ", "")
            f_clean = field_name.replace("_", "").replace(" ", "")
            if f_clean in clean or clean in f_clean:
                native_map[field_name] = m["offset"]
                matched = True
                break
            # 3. Check if the java name's parts are in the method name
            jn_parts = jn_lower.replace("native", "").split("_")
            if all(p in mn_lower for p in jn_parts if p):
                native_map[field_name] = m["offset"]
                matched = True
                break
        if not matched:
            extra.append(m)

    lines = []
    lines.append(f"    /* --- {version} -------------------------------------------------- */")
    lines.append("    {")
    lines.append(f'        "{version}",')
    lines.append("        /* native */ {")
    for field_name, _ in NATIVE_ORDER:
        off = native_map.get(field_name, 0)
        tag = "  /* NOT FOUND */" if off == 0 else ""
        lines.append(f"            0x{off:08x}, /* {field_name:50s} */{tag}")
    lines.append("        },")
    lines.append("        /* surface_changed_pad */ 2,")
    lines.append("        /* global */ {")
    lines.append("            0,         /* LawnApp -- locate me */")
    lines.append("            0,         /* AndroidAppDriver -- locate me */")
    lines.append("        },")
    lines.append("        /* fn */ {0},")

    http_off = next((m["offset"] for m in methods if "http" in m["name"].lower() and "error" in m["name"].lower()), 0)
    lines.append("        /* jni_native */ {")
    lines.append(f"            0x{http_off:08x}, /* http_transaction_error */")
    lines.append("        },")
    lines.append("        /* input */ {},")

    on_draw = native_map.get("on_draw_frame", 0)
    game_init = native_map.get("game_app_initialize", 0)
    fp_draw = fingerprint(data, on_draw)
    fp_init = fingerprint(data, game_init)

    lines.append(f"        0x{fp_draw:016x}ull, /* fingerprint onDrawFrame */")
    lines.append(f"        0x{fp_init:016x}ull, /* fingerprint GameAppInitialize */")
    lines.append("    },")
    lines.append("")
    if extra:
        lines.append("/* Extra methods not in standard order:")
        for m in extra:
            lines.append(f"   0x{m['offset']:08x}  {m['name']:40s}  {m['signature']}")
        lines.append("*/")

    return "\n".join(lines)


# ─── Ghidra mode (optional) ────────────────────────────────────────────────

def open_ghidra(so_path, ghidra_dir=None, analyze=False):
    import pyghidra
    from pyghidra import open_program

    if ghidra_dir:
        os.environ["GHIDRA_INSTALL_DIR"] = str(ghidra_dir)

    ctx_wrapper = open_program(
        str(so_path), project_location=str(so_path.parent),
        project_name="pvz2_re", analyze=analyze,
    )
    api = ctx_wrapper.__enter__()
    return _GhidraContext(ctx_wrapper, api)


class _GhidraContext:
    def __init__(self, ctx_mgr, api):
        self._ctx_mgr = ctx_mgr
        self.api = api
        self.program = api.currentProgram if api else None

    def close(self):
        if self._ctx_mgr:
            self._ctx_mgr.__exit__(None, None, None)
            self._ctx_mgr = None


def verify_with_ghidra(ctx, methods):
    """Use Ghidra decompiler to verify function offsets."""
    if not ctx or not ctx.program:
        return methods

    pgm = ctx.program
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    iface = DecompInterface()
    iface.openProgram(pgm)

    for m in methods:
        addr = pgm.address_factory.getDefaultAddressSpace().getAddress(m["offset"])
        fn = pgm.listing.getFunctionContaining(addr)
        if fn:
            m["ghidra_name"] = fn.name

    return methods


# ─── Main ───────────────────────────────────────────────────────────────────

def main():
    args = parse_args()
    so_path = args.libpvz2

    with open(so_path, "rb") as f:
        data = bytearray(f.read())

    print(f"Image: {len(data)} bytes ({(len(data) + 0xFFFFF) >> 20} MB)", file=sys.stderr)

    # Find JNI_OnLoad
    jni_onload = find_jni_onload_raw(data)
    if jni_onload:
        print(f"JNI_OnLoad at 0x{jni_onload:08x}", file=sys.stderr)
        methods = trace_register_natives(data, jni_onload)
        print(f"  Found {len(methods)} methods via RegisterNatives trace", file=sys.stderr)
    else:
        print("JNI_OnLoad not found in .dynsym", file=sys.stderr)
        methods = []

    # Fallback: scan entire binary
    if not methods:
        print("Scanning entire binary for JNINativeMethod arrays...", file=sys.stderr)
        methods = scan_all_method_arrays(data)
        print(f"  Found {len(methods)} potential methods", file=sys.stderr)

    # Verify with Ghidra if available
    ctx = None
    if args.ghidra_dir:
        print("\nVerifying with Ghidra...", file=sys.stderr)
        try:
            ctx = open_ghidra(so_path, args.ghidra_dir)
            methods = verify_with_ghidra(ctx, methods)
        except Exception as e:
            print(f"  Ghidra verification failed: {e}", file=sys.stderr)

    # Print methods
    print(f"\n{'='*60}", file=sys.stderr)
    print(f"  Native Methods ({len(methods)} found)", file=sys.stderr)
    print(f"{'='*60}", file=sys.stderr)
    for i, m in enumerate(sorted(methods, key=lambda x: x["offset"])):
        gname = m.get("ghidra_name", "")
        extra = f"  → Ghidra: {gname}" if gname else ""
        print(f"  [{i:2d}] 0x{m['offset']:08x}  {m['name']:40s}  {m['signature']}{extra}", file=sys.stderr)

    # Generate symbols.cpp entry
    print(f"\n{'='*60}", file=sys.stderr)
    print(f'  kVersions entry for "{args.name}"', file=sys.stderr)
    print(f"{'='*60}", file=sys.stderr)
    print()
    entry = generate_symbols_entry(methods, args.name, data)
    print(entry)

    # Fingerprints to stderr
    on_draw = next((m["offset"] for m in methods if "draw" in m["name"].lower()), 0)
    game_init = next((m["offset"] for m in methods if "initialize" in m["name"].lower() or "GameApp" in m["name"]), 0)
    print(f"\n  onDrawFrame fingerprint:        0x{fingerprint(data, on_draw):016x}", file=sys.stderr)
    print(f"  GameAppInitialize fingerprint:  0x{fingerprint(data, game_init):016x}", file=sys.stderr)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "version": args.name,
                "image_size": len(data),
                "jni_onload": jni_onload,
                "methods": methods,
            }, f, indent=2)
        print(f"\nWritten: {args.json}", file=sys.stderr)

    if ctx:
        ctx.close()


if __name__ == "__main__":
    main()
