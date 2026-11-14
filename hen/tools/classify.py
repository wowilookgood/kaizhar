# classify.py SHOT REF...: prints how many sampled pixels differ from each reference picture.
import sys, zlib, struct
def load(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i+4])[0]
        if d[i+4:i+8] == b'IDAT': idat += d[i+8:i+8+n]
        i += 12 + n
    raw = zlib.decompress(idat)
    return [raw[y*1201+1:(y+1)*1201] for y in range(480)]
a = load(sys.argv[1])
print(' '.join('%s:%d' % (r.split('/')[-1], sum(1 for y in range(0, 480, 4) for x in range(0, 1200, 12) if abs(a[y][x] - load(r)[y][x]) > 24)) for r in sys.argv[2:]))
