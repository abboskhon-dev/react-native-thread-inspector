#!/usr/bin/env python3
"""Paired differences against the baseline mode ('none' if present, else 'off'), with bootstrap 95% intervals.

Usage: analyze.py <results.csv> [<baseline mode>]

Also prints mean CPU by thread name when <results>.threads.csv sits next to the results.
"""
import csv
import random
import statistics
import sys
from collections import defaultdict
from pathlib import Path

METRICS = [('cpu_ms_per_s', 'CPU ms/s'), ('pss_kb', 'PSS KiB'), ('rss_kb', 'RSS KiB'),
           ('janky_pct', 'janky %'), ('p99_ms', 'p99 frame ms'), ('js_frames_per_s', 'JS frames/s'),
           ('start_ms', 'launch ms'), ('cpu_us_per_frame', 'CPU us/frame')]


def num(v):
    return float(v) if v not in ('', None) else None


def boot_ci(xs, n=5000, seed=7):
    rnd = random.Random(seed)
    means = sorted(statistics.fmean(rnd.choices(xs, k=len(xs))) for _ in range(n))
    return means[int(0.025 * n)], means[int(0.975 * n)]


def thread_table(path, top=8):
    cpu, reps = defaultdict(float), defaultdict(set)
    for r in csv.DictReader(open(path)):
        # Threads sharing a name are summed; the tid column tells them apart.
        cpu[(r['workload'], r['mode'], r['name'])] += float(r['cpu_ms_per_s'])
        reps[(r['workload'], r['mode'])].add(r['rep'])
    for wl in sorted({k[0] for k in reps}):
        modes = sorted(m for w, m in reps if w == wl)
        mean = lambda m, name: cpu[(wl, m, name)] / len(reps[(wl, m)])
        names = {n for w, _, n in list(cpu) if w == wl}
        print(f'\n## {wl}: CPU ms/s by thread name')
        for name in sorted(names, key=lambda n: -max(mean(m, n) for m in modes))[:top]:
            print(f'{name:>16}: ' + ' | '.join(f'{m} {mean(m, name):7.1f}' for m in modes))


def main(path, base=None):
    rows = list(csv.DictReader(open(path)))
    for r in rows:
        cpu, fps = num(r['cpu_ms_per_s']), num(r['js_frames_per_s'])
        # Normalizes for modes completing different amounts of work.
        r['cpu_us_per_frame'] = 1000 * cpu / fps if cpu is not None and fps else None
    by = defaultdict(dict)
    for r in rows:
        by[(r['workload'], r['mode'])][int(r['rep'])] = r
    workloads = sorted({r['workload'] for r in rows})
    base_mode = base or ('none' if any(r['mode'] == 'none' for r in rows) else 'off')
    modes = [m for m in ['off', 'record', 'core', 'full', 'sampling', 'stacks'] if m != base_mode and any(r['mode'] == m for r in rows)]
    for wl in workloads:
        base = by[(wl, base_mode)]
        print(f'\n## {wl} vs {base_mode} (n={len(base)} reps)')
        for key, name in METRICS:
            vals = [num(r[key]) for r in base.values() if num(r[key]) is not None]
            if not vals:
                continue
            line = f'{name:>14}: {base_mode} {statistics.fmean(vals):9.1f}'
            for m in modes:
                diffs = [num(by[(wl, m)][rep][key]) - num(b[key]) for rep, b in base.items()
                         if rep in by[(wl, m)] and num(b[key]) is not None
                         and num(by[(wl, m)][rep][key]) is not None]
                if len(diffs) < 3:
                    continue
                d = statistics.fmean(diffs)
                lo, hi = boot_ci(diffs)
                rel = 100 * d / statistics.fmean(vals) if statistics.fmean(vals) else float('nan')
                line += f' | {m} {d:+8.1f} [{lo:+.1f}, {hi:+.1f}] ({rel:+.1f}%)'
            print(line)
    threads = Path(path).with_suffix('.threads.csv')
    if threads.exists():
        thread_table(threads)


if __name__ == '__main__':
    main(*sys.argv[1:3])
