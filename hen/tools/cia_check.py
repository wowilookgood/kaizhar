# cia_check.py FILE.cia: checks each file of the first content's ExeFS against the hash its table holds.
import hashlib, struct, sys
al64 = lambda x: (x + 63) & ~63
f = open(sys.argv[1], 'rb')
hs, typ, ver, cert, tik, tmd, meta, cont = struct.unpack('<IHHIIIIQ', f.read(0x20))
base = al64(hs) + al64(cert) + al64(tik) + al64(tmd)
f.seek(base); h = f.read(0x200)
unit = 0x200 << h[0x188 + 6]
ex = struct.unpack_from('<I', h, 0x1A0)[0] * unit
f.seek(base + ex); t = f.read(0x200)
good = True
for i in range(10):
    name, off, size = struct.unpack_from('<8sII', t, i * 16)
    if not size: continue
    f.seek(base + ex + 0x200 + off)
    ok = hashlib.sha256(f.read(size)).digest() == t[0x200 - 0x20 * (i + 1):0x200 - 0x20 * i]
    good &= ok
    print(f'  ExeFS {name.rstrip(bytes(1)).decode():8s} {size:9d} bytes: {"ok" if ok else "HASH MISMATCH"}')
sys.exit(0 if good else 1)
