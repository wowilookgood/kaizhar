# session.py LOG: per title started in a title-list session: speed per 5 s and JIT totals.
import re, sys
t = open(sys.argv[1], 'rb').read().replace(b'\r', b'').decode('utf-8', 'replace')
t = re.sub(r'\x1b\[[0-9;]*m', '', t)
for l in re.findall(r'hen: menu: install.*', t): print(l[:230])
parts = re.split(r'hen: loading ', t)[1:]
for p in parts:
    name = p.split('\n')[0].split('/')[-1][:50]
    tid = re.search(r'title/00040000/([0-9a-f]{8})', p.split('\n')[0])
    sp = re.findall(r'hen: perf: speed (\d+)%', p)
    blocks = sum(int(x) for x in re.findall(r'hen: jit compile: (\d+) blocks', p))
    upd = re.findall(r'hen: jit running: (\d+) code updates \(\d+ chunk maps\), ([0-9.]+) ms', p)
    lst = re.findall(r'hen: jit list: (\d+) listed, (\d+) compiled ahead \((\d+) ms at start', p)
    reach = [0] * 5
    for m in re.findall(r'hen: jit reach: .*?<=64K (\d+), <=256K (\d+), <=1M (\d+), <=4M (\d+), further (\d+)', p):
        reach = [a + int(b) for a, b in zip(reach, m)]
    pc = sum(int(a) for a in re.findall(r'pipelines: (\d+) compiled', p)); pm = sum(int(a) for a in re.findall(r'pipelines: \d+ compiled \((\d+) ms', p))
    clears = re.findall(r'(\d+) cache clears', p)
    print(f'== {tid.group(1) if tid else name}: speed {" ".join(sp)}')
    print(f'   JIT: {blocks} blocks compiled while running; code updates {upd[-1][0] if upd else 0} taking {upd[-1][1] if upd else 0} ms; cache clears {clears[-1] if clears else 0}; list at start {lst[0] if lst else None}')
    print(f'   update reach <=64K {reach[0]}, <=256K {reach[1]}, <=1M {reach[2]}, <=4M {reach[3]}, further {reach[4]}; pipelines {pc} compiled in {pm} ms')
    for l in re.findall(r'.*(?:watchdog: no|the program reported|run loop:|<Critical>).*', p)[:3]: print('   ', l[:160])
