#!/usr/bin/env python3
"""shots.py LOG [LOG2] [--png DIR]: the screenshots a run sent in its log.

  shots.py LOG --png DIR     write them as DIR/<log name>-<label>.png
  shots.py LOG LOG2          compare the two runs' shots pair by pair: the share of pixels
                             that differ by more than 8 (of 255) in any channel, and the mean
                             difference; with --png also a picture of the differences

A shot is "hen: shot begin LABEL W H", lines of base64 (RGBA, W*H*4 bytes), "hen: shot end"
(citra_switch/main.cpp, --screenshot-at seconds or --screenshot-frame vertical blanks).
"""
import base64, os, re, struct, sys, zlib


def shots(path):
    t = open(path, 'rb').read().decode('latin-1').replace('\r', '')
    out, head, text = [], None, []
    for line in t.split('\n'):
        if not line.startswith('hen: shot'):
            continue  # (other lines of the log come in between)
        m = re.fullmatch(r'hen: shot begin (\d+) (\d+) (\d+)', line)
        if m:
            head, text = (int(m[1]), int(m[2]), int(m[3])), []
        elif line == 'hen: shot end':
            if head:
                label, w, h = head
                joined = ''.join(text)
                data = base64.b64decode(joined + '=' * (-len(joined) % 4))[:w * h * 4]
                if len(data) == w * h * 4:
                    out.append((label, w, h, data))
            head = None
        elif head:
            text.append(line[10:].strip())
    return out


def png(path, w, h, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    rows = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def opaque(data):
    # (the shot is BGRA; a PNG wants RGBA, and the alpha is of no use here)
    b = bytearray(data)
    b[0::4], b[2::4] = data[2::4], data[0::4]
    b[3::4] = b'\xff' * (len(b) // 4)
    return bytes(b)


def main():
    args = sys.argv[1:]
    out_dir = None
    if '--png' in args:
        i = args.index('--png'); out_dir = args[i + 1]; del args[i:i + 2]
        os.makedirs(out_dir, exist_ok=True)
    name = lambda p: os.path.basename(os.path.dirname(p)) + '-' + os.path.splitext(os.path.basename(p))[0]
    if len(args) == 1:
        for label, w, h, data in shots(args[0]):
            print(f'{name(args[0])}: shot {label}, {w}x{h}')
            if out_dir:
                png(os.path.join(out_dir, f'{name(args[0])}-{label}.png'), w, h, opaque(data))
        return
    a, b = shots(args[0]), shots(args[1])
    if len(a) != len(b):
        print(f'{len(a)} shots against {len(b)}')
    for (la, w, h, da), (lb, w2, h2, db) in zip(a, b):
        if (w, h) != (w2, h2):
            print(f'shot {la}: sizes differ'); continue
        differing = total = 0
        diff = bytearray(w * h * 4)
        for i in range(0, w * h * 4, 4):
            d = max(abs(da[i] - db[i]), abs(da[i + 1] - db[i + 1]), abs(da[i + 2] - db[i + 2]))
            total += d
            if d > 8:
                differing += 1
            diff[i:i + 4] = bytes((min(255, d * 4),) * 3 + (255,))
        print(f'shot {la}/{lb}: {100 * differing / (w * h):6.2f}% of pixels differ by more than 8; mean difference {total / (w * h):.2f}')
        if out_dir:
            png(os.path.join(out_dir, f'{name(args[0])}-{la}.png'), w, h, opaque(da))
            png(os.path.join(out_dir, f'{name(args[1])}-{lb}.png'), w, h, opaque(db))
            png(os.path.join(out_dir, f'diff-{name(args[0])}-{name(args[1])}-{la}.png'), w, h, bytes(diff))


if __name__ == '__main__':
    main()
