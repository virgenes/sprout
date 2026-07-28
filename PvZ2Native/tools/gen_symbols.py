#!/usr/bin/env python3
"""Sprout symbol-table generator.

Given a libPVZ2.so, scans JNI_OnLoad for native method registrations
and produces a kVersions entry ready to paste into symbols.cpp.

Supports ARM and Thumb-2 binaries via capstone disassembly.

Usage:
    python gen_symbols.py path/to/libPVZ2.so [--name "1.0.0"]
"""

import struct
import sys
import os

try:
    from capstone import *
    from capstone.arm import *
    HAS_CAPSTONE = True
except ImportError:
    HAS_CAPSTONE = False


def r32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def find_jni_onload_elf(data):
    """Find JNI_OnLoad offset via ELF .dynsym section."""
    if data[:4] != b'\x7fELF' or data[4] != 1:
        return None

    e_shoff = r32(data, 0x20)
    e_shentsize = r32(data, 0x2E)
    e_shnum = r32(data, 0x30)
    e_shstrndx = r32(data, 0x32)

    if not e_shoff or not e_shnum:
        return None

    # Section name string table
    shdr_str = e_shoff + e_shstrndx * 0x28
    strtab_off = r32(data, shdr_str + 0x10)
    strtab_sz = r32(data, shdr_str + 0x14)
    strtab = data[strtab_off:strtab_off + strtab_sz]

    dynsym_off = dynstr_off = None
    for i in range(e_shnum):
        shdr = e_shoff + i * 0x28
        sh_name = r32(data, shdr)
        name = strtab[sh_name:strtab.index(b'\0', sh_name)].decode('ascii', errors='replace') if sh_name < len(strtab) else ''
        if name == '.dynsym':
            dynsym_off = r32(data, shdr + 0x10)
            dynsym_sz = r32(data, shdr + 0x14)
        elif name == '.dynstr':
            dynstr_off = r32(data, shdr + 0x10)
            dynstr_sz = r32(data, shdr + 0x14)

    if not dynsym_off or not dynstr_off:
        return None

    for i in range(dynsym_sz // 0x10):
        sym = dynsym_off + i * 0x10
        st_name = r32(data, sym)
        st_value = r32(data, sym + 4) & 0xFFFFFF
        if st_name > 0 and st_name < dynstr_sz:
            sym_name = data[dynstr_off + st_name:dynstr_off + dynstr_sz].split(b'\0')[0].decode('ascii', errors='replace')
            if sym_name == 'JNI_OnLoad':
                return st_value
    return None


def find_registernatives_calls(data, fn_addr, fn_size=0x400):
    """Use capstone to find RegisterNatives calls in a function."""
    if not HAS_CAPSTONE:
        return []

    # Try ARM mode first (most PvZ2 functions are ARM)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True

    calls = []
    try:
        code = data[fn_addr:fn_addr + fn_size]
        # Track register state for GOT-relative addressing
        regvals = {}
        got_base = None

        for insn in md.disasm(code, fn_addr):
            ops = insn.operands

            # Track MOV Rd, #imm
            if insn.mnemonic == 'mov' and len(ops) == 2 and ops[1].type == ARM_OP_IMM:
                regvals[ops[0].reg] = ops[1].imm

            # Track LDR Rd, [PC, #imm]
            if insn.mnemonic == 'ldr' and len(ops) >= 2:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC:
                    pool = ((insn.address + 8) & ~3) + (ops[1].mem.disp or 0)
                    if pool < len(data):
                        regvals[rd] = r32(data, pool)

            # Track ADD Rd, PC, Rm (GOT base)
            if insn.mnemonic == 'add' and len(ops) == 3:
                rd = ops[0].reg
                if ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_REG:
                    if ops[1].reg == ARM_REG_PC and ops[2].reg in regvals:
                        got_base = insn.address + 8 + regvals[ops[2].reg]
                        regvals[rd] = got_base

            # Detect BLX Rm (BLX to RegisterNatives)
            if insn.mnemonic in ('blx', 'bl') and len(ops) >= 1:
                target = None
                if ops[0].type == ARM_OP_REG and ops[0].reg in regvals:
                    target = regvals[ops[0].reg]
                elif ops[0].type == ARM_OP_IMM:
                    target = ops[0].imm

                # The gMethods array is usually in r1 or r2 before the call
                # Look back for LDR r1/r2 from [got_base + offset]
                gmethods_addr = None
                for back_off in range(4, 0x40, 4):
                    prev_addr = insn.address - back_off
                    if prev_addr < fn_addr:
                        break
                    # Can't re-disassemble backwards easily, check raw
                    prev_instr = r32(data, prev_addr)
                    # ARM LDR R1, [R3, #imm] or LDR R2, [R3, #imm]
                    if (prev_instr & 0xFE000000) == 0xE4000000 and ((prev_instr >> 12) & 0xF) in (1, 2):
                        rn = (prev_instr >> 16) & 0xF
                        if rn in regvals and regvals[rn] >= 0x1000000:
                            imm12 = prev_instr & 0xFFF
                            gmethods_addr = regvals[rn] + imm12
                            break

                if gmethods_addr and gmethods_addr < len(data):
                    calls.append(gmethods_addr)

    except Exception:
        pass

    return calls


def read_jni_methods(data, array_offset):
    """Read a JNINativeMethod array until null terminator."""
    entries = []
    off = array_offset
    while off + 12 <= len(data):
        name_ptr = r32(data, off)
        sig_ptr = r32(data, off + 4)
        fn_ptr = r32(data, off + 8)
        if name_ptr == 0 and sig_ptr == 0 and fn_ptr == 0:
            break
        name = ""
        if name_ptr < len(data):
            end = data.index(b'\0', name_ptr) if b'\0' in data[name_ptr:name_ptr + 128] else len(data)
            name = data[name_ptr:end].decode('ascii', errors='replace')
        sig = ""
        if sig_ptr < len(data):
            end = data.index(b'\0', sig_ptr) if b'\0' in data[sig_ptr:sig_ptr + 128] else len(data)
            sig = data[sig_ptr:end].decode('ascii', errors='replace')
        fn_offset = fn_ptr if fn_ptr < len(data) else 0
        entries.append({"name": name, "signature": sig, "offset": fn_offset})
        off += 12
    return entries


def scan_all_method_arrays(data):
    """Scan for JNINativeMethod arrays by looking for string pointers."""
    arrays = []
    # JNINativeMethod is 12 bytes: {name_ptr, sig_ptr, fn_ptr}
    # Both name_ptr and sig_ptr should point to .rodata strings
    for off in range(0x100000, len(data) - 12, 4):
        name_ptr = r32(data, off)
        sig_ptr = r32(data, off + 4)
        fn_ptr = r32(data, off + 8)
        if (name_ptr == 0 and sig_ptr == 0 and fn_ptr == 0):
            continue
        if (name_ptr < len(data) and sig_ptr < len(data) and fn_ptr < len(data) and
                name_ptr > 0x1000000 and sig_ptr > 0x1000000):
            # Check if name and sig look like strings
            try:
                name_end = data.index(b'\0', name_ptr) if b'\0' in data[name_ptr:name_ptr + 64] else name_ptr
                sig_end = data.index(b'\0', sig_ptr) if b'\0' in data[sig_ptr:sig_ptr + 64] else sig_ptr
                name = data[name_ptr:name_end].decode('ascii', errors='replace')
                sig = data[sig_ptr:sig_end].decode('ascii', errors='replace')
                if name and sig and sig.startswith('(') and len(name) > 2 and len(name) < 80:
                    arrays.append(off)
            except Exception:
                pass
    return arrays


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"error: '{path}' not found")
        sys.exit(1)

    version_name = "X.Y.Z"
    if "--name" in sys.argv:
        idx = sys.argv.index("--name")
        if idx + 1 < len(sys.argv):
            version_name = sys.argv[idx + 1]

    with open(path, "rb") as f:
        data = f.read()

    img_size = len(data)
    print(f"Image: {img_size} bytes ({(img_size + 0xFFFFF) >> 20} MB)")
    print()

    # Find JNI_OnLoad
    jni_onload = find_jni_onload_elf(data)

    # Try to find RegisterNatives calls
    all_methods = []
    if jni_onload:
        print(f"JNI_OnLoad at 0x{jni_onload:08x}")
        arrays = find_registernatives_calls(data, jni_onload)
        for arr_off in arrays:
            methods = read_jni_methods(data, arr_off)
            all_methods.extend(methods)

    # If capstone failed, scan for arrays directly
    if not all_methods:
        print("(falling back to direct array scan)")
        array_offsets = scan_all_method_arrays(data)
        for arr_off in array_offsets[:5]:  # limit to avoid false positives
            methods = read_jni_methods(data, arr_off)
            if len(methods) >= 3:
                all_methods.extend(methods)

    # Deduplicate by name
    seen_names = set()
    unique = []
    for m in all_methods:
        if m["name"] and m["name"] not in seen_names:
            seen_names.add(m["name"])
            unique.append(m)
    all_methods = unique

    if all_methods:
        print(f"\nFound {len(all_methods)} JNI native methods:\n")
        for i, m in enumerate(all_methods):
            print(f"  [{i:2d}] 0x{m['offset']:08x}  {m['name']:40s}  {m['signature']}")
        print()

    # Map to known method order
    native_order = [
        "nativeInitialize",
        "applicationWillFinishLaunching",
        "applicationDidFinishLaunching",
        "applicationWillBecomeForeground",
        "applicationDidBecomeActive",
        "onSurfaceCreated",
        "onSurfaceChanged",
        "onDrawFrame",
    ]
    native_offsets = [0] * 8
    for m in all_methods:
        for i, name in enumerate(native_order):
            if name in m["name"] or m["name"] in name:
                native_offsets[i] = m["offset"]

    # Generate kVersions entry
    print("=" * 60)
    print(f"kVersions entry for \"{version_name}\":")
    print("=" * 60)
    print()
    print("    /* ---", version_name, "-------------------------------------------------- */")
    print("    {")
    print(f'        "{version_name}",')
    print("        /* native */ {")
    for i, name in enumerate(native_order):
        off = native_offsets[i]
        tag = "  /* NOT FOUND */" if off == 0 else ""
        print(f"            0x{off:08x}, /* {name:45s} */{tag}")
    print("        },")
    print("        /* surface_changed_pad */ 2,")
    print("        /* global */ {")
    print("            0,         /* LawnApp -- locate me */")
    print("            0,         /* AndroidAppDriver -- locate me */")
    print("        },")
    print("        /* fn */ {0},")
    print("        /* jni_native */ {0},")
    print("        /* input */ {},")

    # Get fingerprints from first 8 bytes of key functions
    fps = {}
    for idx, label in [(7, "on_draw_frame"), (0, "game_app_initialize")]:
        off = native_offsets[idx] if idx < len(native_offsets) else 0
        if off and off + 8 <= len(data):
            fps[label] = struct.unpack("<Q", data[off:off + 8])[0]
        else:
            fps[label] = 0

    print(f"        0x{fps['on_draw_frame']:016x}ull, /* fingerprint onDrawFrame */")
    print(f"        0x{fps['game_app_initialize']:016x}ull, /* fingerprint GameAppInitialize */")
    print("    },")

    print()
    print("Raw fingerprints:")
    print(f"  onDrawFrame:        0x{fps['on_draw_frame']:016x}")
    print(f"  GameAppInitialize:  0x{fps['game_app_initialize']:016x}")


if __name__ == "__main__":
    main()
