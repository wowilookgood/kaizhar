# perf.py LOG: one-paragraph summary of the run log of a development build.
import re, sys
t = open(sys.argv[1], 'rb').read().replace(b'\r', b'').decode('utf-8', 'replace')
t = re.sub(r'\x1b\[[0-9;]*m', '', t)
t = '\n'.join(l for l in t.split('\n') if not l.startswith('hen: shot '))
sp = [int(x) for x in re.findall(r'hen: perf: speed (\d+)%', t)]
fps = re.findall(r'hen: perf: speed \d+%, game ([0-9.]+)', t)
n = ms = longest = waits = wms = cached = 0
for m in re.finditer(r'pipelines: (\d+) compiled \((\d+) ms, longest (\d+)\), (\d+) from the driver.*?render thread waited (\d+) times \((\d+) ms', t):
    n += int(m[1]); ms += int(m[2]); longest = max(longest, int(m[3])); cached += int(m[4]); waits += int(m[5]); wms += int(m[6])
comp = sum(int(a) for a in re.findall(r'hen: jit compile: (\d+) blocks', t))
lst = re.findall(r'hen: jit list: (\d+) listed, (\d+) compiled ahead \((\d+) ms at start', t)
print('speed % per 5 s:', ' '.join(map(str, sp)))
print('game fps:', ' '.join(fps))
print(f'pipelines: {n} compiled, {ms} ms ({ms / max(n, 1):.0f} ms each, longest {longest}), {cached} from the driver cache; render thread waited {waits} times, {wms} ms')
print(f'JIT: {comp} blocks compiled while running; list at start: {lst[0] if lst else None}')
for pat in ('watchdog: (no|pc)', 'terminate', 'the program reported', 'run loop:', 'hen: --title'):
    for l in re.findall(r'.*' + pat + r'.*', t)[:2]: print('  ', l[:170])
print('exited cleanly' if 'hen: exiting' in t else 'DID NOT EXIT CLEANLY: ' + ' | '.join(t.strip().split('\n')[-2:])[:200])
