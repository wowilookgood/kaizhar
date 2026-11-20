#!/usr/bin/env python3
"""Drives the test instances: ctl.py A|B|AB command...   (commands: see hid_test_input.h)
   ctl.py A shot NAME     takes a picture and converts it to NAME.png
   ctl.py A wait MS       waits until MS more milliseconds of emulated time have passed
   ctl.py A status"""
import os, struct, sys, time, zlib
# The work folder: the two instances' user folders (A, B) and the pictures (shots) are in it.
ROOT = os.environ.get("KAIZHAR_TEST_ROOT") or sys.exit("set KAIZHAR_TEST_ROOT to the test's work folder (README.md)")

def status(inst):
    try:
        return dict(l.split(None, 1) for l in open(f"{ROOT}/{inst}/ctl/status").read().splitlines())
    except Exception:
        return {}

def send(inst, line):
    path = f"{ROOT}/{inst}/ctl/cmd"
    for _ in range(200):
        if not os.path.exists(path):
            break
        time.sleep(0.02)
    with open(path + ".tmp", "w") as f:
        f.write(line + "\n")
    os.rename(path + ".tmp", path)

def wait(inst, ms, limit=120):
    start = int(status(inst).get("ms", 0))
    t0 = time.time()
    while time.time() - t0 < limit:
        if int(status(inst).get("ms", 0)) - start >= ms:
            return True
        time.sleep(0.05)
    return False

def png(raw, out):
    d = open(raw, "rb").read(); nl = d.index(b"\n"); w, h = map(int, d[:nl].split()); px = d[nl + 1:]
    rows = b"".join(b"\x00" + px[y * w * 4:(y + 1) * w * 4] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b""))

def pair(name):
    """Both instances' screens side by side in one picture."""
    raws = []
    for inst in "AB":
        raw = f"{ROOT}/{inst}/ctl/{name}.raw"
        if os.path.exists(raw):
            os.remove(raw)
        send(inst, f"shot {name}")
        raws.append(raw)
    for raw in raws:
        for _ in range(300):
            if os.path.exists(raw):
                break
            time.sleep(0.05)
    imgs = []
    for raw in raws:
        d = open(raw, "rb").read(); nl = d.index(b"\n"); w, h = map(int, d[:nl].split()); imgs.append((w, h, d[nl + 1:]))
    w, h = imgs[0][0], imgs[0][1]
    gap = b"\xff\xff\xff\xff" * 8
    rows = b"".join(b"\x00" + imgs[0][2][y * w * 4:(y + 1) * w * 4] + gap + imgs[1][2][y * w * 4:(y + 1) * w * 4] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    out = f"{ROOT}/shots/pair-{name}.png"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", 2 * w + 8, h, 8, 6, 0, 0, 0)) + ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b""))
    return out

def shot(inst, name):
    raw = f"{ROOT}/{inst}/ctl/{name}.raw"
    if os.path.exists(raw):
        os.remove(raw)
    send(inst, f"shot {name}")
    for _ in range(300):
        if os.path.exists(raw):
            break
        time.sleep(0.05)
    out = f"{ROOT}/shots/{inst}-{name}.png"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    png(raw, out)
    return out

if __name__ == "__main__":
    insts, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    if cmd == "pair":
        print(pair(args[0])); sys.exit(0)
    for inst in insts:
        if cmd == "shot":
            print(shot(inst, args[0]))
        elif cmd == "wait":
            print(inst, wait(inst, int(args[0])))
        elif cmd == "status":
            print(inst, status(inst))
        else:
            send(inst, " ".join([cmd] + args))
