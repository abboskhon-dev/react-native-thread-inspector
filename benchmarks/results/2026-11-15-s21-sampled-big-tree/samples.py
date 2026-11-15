import json, sys, os, collections
raw = sys.argv[1]; pairs = [p.split(':') for p in sys.argv[2:]]
tot = collections.Counter(); totn = 0
for inc, ses in pairs:
    rep = json.load(open(f'{raw}/{inc}/incident.json'))
    prof = json.load(open(f'{raw}/{ses}/hermes.cpuprofile'))
    sf = prof['stackFrames']
    js = [t for t in rep['triggers'] if t['source'].startswith('js')][0]
    on, end, tid = js['onset_ns'], js['end_ns'], str(js['tid'])
    spans = [f for f in rep['analysis'][rep['triggers'].index(js)]['findings'] if f['relation'] == 'direct_containment']
    def stack(i):
        out = []
        while i and str(i) in sf:
            out.append(sf[str(i)]['name']); i = sf[str(i)].get('parent')
        return out  # leaf first
    S = sorted((int(s['ts']) * 1000, stack(s['sf'])) for s in prof['samples'] if s['tid'] == tid)
    ins = [(t, st) for t, st in S if on <= t <= end]
    before = [t for t, _ in S if t < on]; after = [t for t, _ in S if t > end]
    print(f'{inc}: stall {(end-on)/1e6:.1f} ms, {len(ins)} samples inside')
    print(f'  last sample before onset: {(on-before[-1])/1e6:.1f} ms before; first inside +{(ins[0][0]-on)/1e6:.1f}; last inside {(end-ins[-1][0])/1e6:.1f} before end; next after +{(after[0]-end)/1e6:.1f}')
    for f in spans:
        n = sum(f['start_ns'] <= t <= f['end_ns'] for t, _ in ins)
        print(f"  span {f['label'][:40]} [{(f['start_ns']-on)/1e6:.1f}, {(f['end_ns']-on)/1e6:.1f}] ms: {n} samples")
    un = [(t, st) for t, st in ins if not any(f['start_ns'] <= t <= f['end_ns'] for f in spans)]
    def bucket(st):
        names = set(st)
        leaf = st[0]
        if leaf.startswith('[HostFunction]') or leaf.startswith('[Native]'): return leaf
        if 'commitRoot' in names: return 'React commit (JS)'
        if 'renderRootSync' in names: return 'React render (JS)'
        return 'other JS: ' + leaf
    c = collections.Counter(bucket(st) for _, st in un)
    print(f'  outside spans: {len(un)} samples')
    for k, v in c.most_common(): print(f'    {v:3d}  {k}')
    tot.update(c); totn += len(un)
    for t, st in un[:1] + un[-1:]: print('    e.g.', ' < '.join(st[:6]))
print('ALL outside-span samples', totn)
for k, v in tot.most_common(): print(f'  {v:3d} ({v/totn:.0%})  {k}')
