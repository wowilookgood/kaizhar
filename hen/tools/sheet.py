# sheet.py OUT.png IN.png...: the screenshots side by side.
import sys, zlib, struct
def load(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i+4])[0]
        if d[i+4:i+8] == b'IDAT': idat += d[i+8:i+8+n]
        i += 12 + n
    raw = zlib.decompress(idat); return [raw[y*1201+1:(y+1)*1201] for y in range(480)]
imgs = [load(p) for p in sys.argv[2:]]
rows = b''.join(b'\0' + b''.join(im[y] for im in imgs) for y in range(480))
def chunk(k, d): return struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d))
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 400 * len(imgs), 480, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
