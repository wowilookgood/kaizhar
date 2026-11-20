#!/usr/bin/env python3
"""dis.py CODE.bin START END  : ARM disassembly of the dumped program (loaded at 0x100000)"""
import sys, struct, capstone
code = open(sys.argv[1], "rb").read(); base = 0x100000
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
for off in range(start, end, 4):
    w = code[off - base: off - base + 4]
    ins = list(md.disasm(w, off))
    if ins:
        i = ins[0]; extra = ""
        if "[pc" in i.op_str and i.mnemonic.startswith("ldr"):
            try:
                imm = int(i.op_str.split("#")[1].rstrip("]"), 16) if "#" in i.op_str else 0
                if "-" in i.op_str.split("pc")[1][:4]: imm = -imm
                v = struct.unpack_from("<I", code, off + 8 + imm - base)[0]; extra = f"   ; ={v:#x}"
            except Exception: pass
        print(f"{off:08x}: {i.mnemonic:8s} {i.op_str}{extra}")
    else:
        print(f"{off:08x}: .word {struct.unpack('<I', w)[0]:#010x}")
