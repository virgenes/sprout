#!/usr/bin/env python3
"""ghidra_vtables.py — Scan ARM32 vtable hierarchy in libPVZ2.so.

Vtables in ARM32 are arrays of function pointers (4 bytes each) that start
at well-aligned addresses. The first entry is the "complete object destructor"
or a typeinfo pointer. By scanning for aligned function-pointer arrays, we can:

  1. Locate all vtables in the binary
  2. Estimate object sizes (vtables often precede their data in memory,
     or adjacent vtables suggest related classes)
  3. Map inheritance relationships (a derived class vtable often points
     to a base class vtable + delta)

Output: list of vtables with their address, length, and called functions.

Usage:
    python ghidra_vtables.py path/to/libPVZ2.so [--ghidra-dir DIR] [--json FILE]
"""

import argparse
import json
import struct
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="ARM32 vtable scanner")
    parser.add_argument("libpvz2", type=Path, help="Path to libPVZ2.so")
    parser.add_argument("--ghidra-dir", default=None)
    parser.add_argument("--json", type=Path, default=None, help="Output JSON")
    parser.add_argument("--min-vtable", type=int, default=4,
                        help="Minimum vtable entries (default: 4)")
    parser.add_argument("--max-vtable", type=int, default=500,
                        help="Maximum vtable entries (default: 500)")
    parser.add_argument("--section", default=None,
                        help="Target section (default: .rodata or .data)")
    parser.add_argument("--no-analysis", action="store_true")
    return parser.parse_args()


def get_section_ranges(data: bytearray) -> list:
    """Get section vaddr ranges from ELF section headers."""
    if data[:4] != b"\x7fELF":
        return [{"name": "whole_image", "start": 0, "end": len(data)}]

    is_32 = data[4] == 1
    e_shoff = struct.unpack_from("<I", data, 0x20)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x2E if is_32 else 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x30 if is_32 else 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x32 if is_32 else 0x3E)[0]
    shstrtab_off = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 0x10)[0]
    shstrtab = data[shstrtab_off:]

    sections = []
    for i in range(e_shnum):
        shdr = e_shoff + i * e_shentsize
        sh_name = struct.unpack_from("<I", data, shdr)[0]
        sh_type = struct.unpack_from("<I", data, shdr + 0x04)[0]
        sh_addr = struct.unpack_from("<I", data, shdr + 0x0C)[0]
        sh_size = struct.unpack_from("<I", data, shdr + 0x10)[0]
        name_end = shstrtab.index(b"\x00", sh_name) if b"\x00" in shstrtab[sh_name:] else len(shstrtab)
        name = shstrtab[sh_name:name_end].decode("ascii", errors="replace")

        if sh_addr and sh_size:
            sections.append({"name": name, "start": sh_addr, "end": sh_addr + sh_size, "type": sh_type})

    return sections


def is_valid_ptr(val: int, data_len: int) -> bool:
    """Check if a value looks like a valid code pointer."""
    if val < 0x100000 or val >= data_len:
        return False
    # Should be in a code-like section — check for ARM/Thumb instruction patterns
    return True


def scan_vtables(data: bytearray, sections: list, min_entries: int, max_entries: int,
                 targets: list = None) -> list:
    """Scan for vtable-like structures in data sections."""
    vtables = []

    # Filter to data sections that can contain vtables
    target_sections = []
    if targets:
        target_sections = [s for s in sections if s["name"] in targets]
    else:
        target_sections = [s for s in sections if s["name"] in (".rodata", ".data", ".bss")]

    if not target_sections:
        target_sections = sections

    for sec in target_sections:
        start = max(0x100000, sec["start"])
        end = min(len(data) - 4, sec["end"])

        # Scan on 4-byte alignment
        for off in range(start, end, 4):
            # Check stride: vtables have multiple consecutive valid function pointers
            count = 0
            for i in range(max_entries):
                ptr = struct.unpack_from("<I", data, off + i * 4)[0]
                if is_valid_ptr(ptr, len(data)):
                    # Check it's ARM code (not Thumb — ARM functions are even-addressed)
                    # Thumb functions have LSB=1, ARM have LSB=0
                    if ptr & 1:
                        ptr_thumb = ptr & ~1
                        if ptr_thumb >= len(data):
                            break
                    # Check if this looks like actual code (common ARM instruction patterns)
                    if ptr >= 0x100000 and ptr < sec["start"] + 0x200000:
                        count += 1
                    else:
                        break
                else:
                    break

            if count >= min_entries:
                # Check this isn't inside another vtable we already found
                already_found = False
                for v in vtables:
                    if v["offset"] <= off < v["offset"] + v["count"] * 4:
                        already_found = True
                        break

                if not already_found:
                    vtables.append({
                        "offset": off,
                        "count": count,
                        "section": sec["name"],
                        "estimated_size": count * 4,
                    })

    return vtables


def analyze_vtable(data: bytearray, vtable: dict) -> dict:
    """Analyze a single vtable — read function pointers and try to identify class."""
    off = vtable["offset"]
    functions = []

    for i in range(vtable["count"]):
        ptr = struct.unpack_from("<I", data, off + i * 4)[0]
        is_thumb = bool(ptr & 1)
        target = ptr & ~1

        functions.append({
            "slot": i,
            "address": ptr,
            "target": target,
            "is_thumb": is_thumb,
        })

    return {
        "offset": vtable["offset"],
        "count": vtable["count"],
        "section": vtable["section"],
        "functions": functions[:20],  # First 20 entries for inspection
    }


def guess_class_name(vtable: dict, sorted_vtables: list) -> str:
    """Heuristic: name vtables by their position and size."""
    off = vtable["offset"]
    count = vtable["count"]

    # Check for common PvZ2 class patterns
    if count > 100:
        return f"large_class_0x{off:x}_vtable({count})"
    if count > 40:
        return f"medium_class_0x{off:x}_vtable({count})"

    # Check if adjacent to another vtable (possible base/derived pair)
    for v2 in sorted_vtables:
        if v2 is vtable:
            continue
        delta = abs(v2["offset"] - off)
        if delta < 64 and delta > 0:
            return f"related_0x{v2['offset']:x}-0x{off:x}"
    return f"class_0x{off:x}_vtable({count})"


def main():
    args = parse_args()
    so_path = args.libpvz2

    with open(so_path, "rb") as f:
        data = bytearray(f.read())

    sections = get_section_ranges(data)
    print(f"Sections found: {len(sections)}", file=sys.stderr)

    targets = [args.section] if args.section else None
    vtables = scan_vtables(data, sections, args.min_vtable, args.max_vtable, targets)

    # Sort by offset
    vtables.sort(key=lambda v: v["offset"])

    print(f"\nFound {len(vtables)} vtables:\n", file=sys.stderr)
    print(f"{'Offset':>12}  {'Section':>12}  {'Entries':>7}  {'Size':>8}  {'Guess'}", file=sys.stderr)
    print(f"{'─'*12}  {'─'*12}  {'─'*7}  {'─'*8}  {'─'*30}", file=sys.stderr)

    for v in vtables[:100]:  # Limit output
        name = guess_class_name(v, vtables)
        print(f"  0x{v['offset']:08x}  {v['section']:>12}  {v['count']:>7}  {v['estimated_size']:>8}  {name}",
              file=sys.stderr)

    if len(vtables) > 100:
        print(f"  ... and {len(vtables) - 100} more", file=sys.stderr)

    # Analyze vtables in Ghidra for deeper inspection
    print("\n=== Ghidra Decompilation of Key Vtables ===\n", file=sys.stderr)

    if not args.no_analysis:
        import pyghidra
        kwargs = {"verbose": True}
        if args.ghidra_dir:
            kwargs["install_dir"] = args.ghidra_dir
        launcher = pyghidra.HeadlessPyGhidraLauncher(**kwargs)
        launcher.start()
        ctx = launcher.open_program(
            str(so_path), analyze=True,
            project_name="pvz2_re", project_location=str(so_path.parent),
        )
        pgm = ctx.program

        from ghidra.app.decompiler import DecompInterface
        from ghidra.util.task import ConsoleTaskMonitor
        iface = DecompInterface()
        iface.openProgram(pgm)

        # Decompile the first function in a few representative vtables
        for v in vtables[:5]:
            if v["count"] > 0:
                fn_ptr = struct.unpack_from("<I", data, v["offset"])[0] & ~1
                addr = pgm.address_factory.getDefaultAddressSpace().getAddress(fn_ptr)
                fn = pgm.listing.getFunctionContaining(addr)
                if fn:
                    res = iface.decompileFunction(fn, 0, ConsoleTaskMonitor())
                    if res and res.decompileCompleted():
                        name = guess_class_name(v, vtables)
                        print(f"\n--- {name} (first vtable entry at 0x{fn_ptr:08x}) ---",
                              file=sys.stderr)
                        decomp = res.getDecompiledFunction().getC()
                        for line in decomp.split("\n")[:15]:
                            print(f"  {line}", file=sys.stderr)

        ctx.close()

    # Output summary JSON
    if args.json:
        detailed = []
        for v in vtables[:50]:
            detailed.append(analyze_vtable(data, v))
        with open(args.json, "w") as f:
            json.dump({
                "total_vtables": len(vtables),
                "sections": sections,
                "vtables": detailed,
            }, f, indent=2)
        print(f"\nWrote {args.json}", file=sys.stderr)


if __name__ == "__main__":
    main()
