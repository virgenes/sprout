#!/usr/bin/env python3
"""Find LawnApp offset for 4.5.2 by tracing operator new calls in GameAppInitialize.

GameAppInitialize creates a LawnApp object. We find the call to operator new,
track the return value to a STR instruction, and that BSS address is LawnApp.
"""

import struct, sys
from capstone import *
from capstone.arm import *

def r32(d, o):
    return struct.unpack_from("<I", d, o)[0]

def is_bss(addr):
    return 0x1159aa0 <= addr <= 0x11d53f4

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        d = f.read()

    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True

    BSS_LO, BSS_HI = 0x1159aa0, 0x11d53f4

    # GameAppInitialize at 0xcc033c, ~0x500 bytes
    fn_addr, fn_size = 0xcc033c, 0x600
    code = d[fn_addr:fn_addr+fn_size]

    print("=" * 70)
    print("  FINDING LawnApp in GameAppInitialize (4.5.2)")
    print("=" * 70)
    print()

    # Track register values through the function
    regs = {}           # current known values
    got_base = None     # GOT base address
    str_to_bss = []     # (address, dest_bss, src_reg_desc)
    call_sites = []     # (address, target, args_desc)

    seen_blocks = set()

    for insn in md.disasm(code, fn_addr):
        ops = insn.operands
        addr = insn.address
        mnem = insn.mnemonic

        # === TRACK MOV Rd, #imm ===
        if mnem == 'mov' and len(ops) == 2 and ops[1].type == ARM_OP_IMM:
            regs[ops[0].reg] = ops[1].imm

        # === TRACK LDR Rd, [PC, #imm] (literal pool) ===
        if mnem == 'ldr' and len(ops) >= 2:
            rd = ops[0].reg
            mem = ops[1]
            if mem.type == ARM_OP_MEM and mem.mem.base == ARM_REG_PC:
                pool = ((addr + 8) & ~3) + (mem.mem.disp or 0)
                if pool < len(d):
                    regs[rd] = r32(d, pool)

        # === TRACK ADD Rd, PC, Rm (GOT base calc) ===
        if mnem == 'add' and len(ops) == 3:
            rd = ops[0].reg
            if ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_REG:
                base_r = ops[1].reg
                off_r = ops[2].reg
                if base_r == ARM_REG_PC and off_r in regs:
                    got_base = addr + 8 + regs[off_r]
                    regs[rd] = got_base
                # Also track ADD Rd, Rd, #imm
                if base_r == rd and ops[2].type == ARM_OP_IMM:
                    if rd in regs:
                        regs[rd] = regs.get(rd, 0) + ops[2].imm

        # === TRACK LDR from GOT (ldr Rd, [Rn, #imm]) ===
        if mnem == 'ldr' and len(ops) >= 2:
            rd = ops[0].reg
            if ops[1].type == ARM_OP_MEM:
                base = ops[1].mem.base
                disp = ops[1].mem.disp or 0
                if base in regs:
                    load_addr = regs[base] + disp
                    if load_addr < len(d):
                        regs[rd] = r32(d, load_addr)
                    elif is_bss(load_addr):
                        # BSS read - loaded value unknown at compile time
                        regs.pop(rd, None)

        # === TRACK LDMIA (load multiple) ===
        if mnem == 'ldmia' and len(ops) >= 2:
            base_r = ops[0].reg
            if base_r == ARM_REG_SP:
                for i, op in enumerate(ops[1:]):
                    if hasattr(op, 'reg'):
                        regs[op.reg] = f"stack:{i}"

        # === DETECT CALLS ===
        if mnem in ('bl', 'blx'):
            target = None
            call_desc = ""
            if ops[0].type == ARM_OP_IMM:
                target = ops[0].imm
                call_desc = f"0x{target:08x}"
            elif ops[0].type == ARM_OP_REG and ops[0].reg in regs:
                target = regs[ops[0].reg]
                call_desc = f"r{ops[0].reg}=0x{target:08x}"

            # Describe args (r0-r3)
            args = []
            for r in range(4):
                if r in regs:
                    args.append(f"r{r}=0x{regs[r]:08x}" if isinstance(regs[r], int) else f"r{r}={regs[r]}")
                else:
                    args.append(f"r{r}=?")

            call_sites.append((addr, target, args, mnem))
            regs.pop(0, None)  # r0 = return value (unknown)

        # === DETECT STR to BSS ===
        if mnem.startswith('str') and len(ops) >= 2:
            rd = ops[0]
            mem = ops[1]
            if mem.type == ARM_OP_MEM:
                base = mem.mem.base
                disp = mem.mem.disp or 0
                # Compute target address
                if base in regs and isinstance(regs[base], int):
                    target = regs[base] + disp
                    if is_bss(target):
                        src_desc = f"r{rd.reg}" if rd.type == ARM_OP_REG else "?"
                        src_val = regs.get(rd.reg, 0) if rd.type == ARM_OP_REG else 0
                        str_to_bss.append((addr, target, src_desc, src_val, f"[r{base}, #0x{disp:x}]"))

    # === OUTPUT ===
    print("All calls in GameAppInitialize:\n")
    for addr, target, args, mnem in call_sites:
        print(f"  0x{addr:08x}: {mnem}  {args[0]}, {args[1]}, {args[2]}, {args[3]}")
        if target and isinstance(target, int):
            # Check if target looks like operator new (Znwj) or new[]
            # Common values for operator new in ARM: _Znwj, _Znaj
            print(f"               target = 0x{target:08x}")

    print(f"\nSTR to BSS addresses in GameAppInitialize:\n")
    if str_to_bss:
        for addr, target, src_desc, src_val, op_str in str_to_bss:
            label = ""
            if abs(target - 0x117a734) < 0x10000:
                label = "  <<< NEAR ANDROIDAPPDRIVER"
            if abs(target - 0x117a5b0) < 0x100:
                label = "  <<< LIKELY LawnApp (near driver)"
            print(f"  0x{addr:08x}: STR {src_desc} -> {op_str} = 0x{target:08x}{label}")

    # Try to identify which call creates LawnApp by looking at allocation size
    print(f"\n=== Analysis: IDENTIFYING LawnApp ===")
    print(f"""
  The LawnApp is created by operator new(size) in GameAppInitialize.
  LawnApp is ~0x100+ bytes. Look for the call where:
  1. r0 (size) is a large value (> 0x100)
  2. The return value (r0) is later STR-ed to BSS
  3. That BSS address is NOT AndroidAppDriver (0x117a734)
  
  Shortlist of nearby BSS addresses:""")

    # Show key candidates
    for addr, target, src_desc, src_val, op_str in sorted(str_to_bss, key=lambda x: x[1]):
        if abs(target - 0x117a734) < 0x20000:
            print(f"    0x{target:08x}  written at 0x{addr:08x}")

    print(f"""
  If none of the above shows the BSS target, the function uses
  GOT indirect addressing (ldr rX, [sb, rY]) which requires
  runtime analysis with a debugger (x64dbg on sprout.exe).
  
  RECOMMENDATION: Install Ghidra, open this .so, decompile
  GameAppInitialize (0xcc033c), and look for:
    - _Znwj (operator new) call with large size
    - The BSS address the return value gets stored to
""")

if __name__ == "__main__":
    main()
