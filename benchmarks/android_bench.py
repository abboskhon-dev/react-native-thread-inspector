#!/usr/bin/env python3
"""Paired, randomized overhead runs of the example app on one Android device.

Measures from outside the app (adb), so the recorder never observes itself:
process CPU from /proc/<pid>/stat, RSS from /proc/<pid>/status, PSS from
dumpsys meminfo, frame stats from dumpsys gfxinfo, JS throughput from logcat.
"""
import argparse
import csv
import json
import random
import re
import subprocess
import time
from pathlib import Path

PKG = 'threadinspector.example'
MODES = ['none', 'off', 'record', 'core', 'full', 'sampling']
WORKLOADS = ['idle', 'active']
FIELDS = ['rep', 'order', 'mode', 'workload', 'start_ms', 'window_s', 'cpu_ms_per_s',
          'rss_kb', 'pss_kb', 'frames', 'janky_pct', 'p90_ms', 'p99_ms', 'js_frames_per_s',
          'spans_per_s', 'native_calls_per_s', 'battery_temp_c']


def adb(*args, check=True):
    return subprocess.run(['adb', *args], capture_output=True, text=True, check=check).stdout


def sh(cmd):
    return adb('shell', cmd)


def cpu_ticks(pid):
    f = sh(f'cat /proc/{pid}/stat').rsplit(')', 1)[1].split()
    return int(f[11]) + int(f[12])  # utime + stime, after the comm field


def status_kb(pid, key):
    m = re.search(rf'^{key}:\s+(\d+)', sh(f'cat /proc/{pid}/status'), re.M)
    return int(m.group(1)) if m else None


def pss_kb():
    m = re.search(r'TOTAL PSS:\s+(\d+)', sh(f'dumpsys meminfo {PKG}'))
    return int(m.group(1)) if m else None


def gfx():
    out = sh(f'dumpsys gfxinfo {PKG}')
    def num(pat):
        m = re.search(pat, out, re.M)
        return float(m.group(1)) if m else None
    return {'frames': num(r'Total frames rendered: (\d+)'),
            'janky_pct': num(r'Janky frames: \d+ \(([\d.]+)%\)'),
            'p90_ms': num(r'^90th percentile: (\d+)ms'),
            'p99_ms': num(r'^99th percentile: (\d+)ms')}


def bench_lines():
    out = adb('logcat', '-d', '-s', 'ReactNativeJS:I')
    return [json.loads(m.group(1)) for m in re.finditer(r'RNTI_BENCH (\{.*\})', out)]


def rate(lines, key, t0, t1):
    inside = [l for l in lines if t0 <= l['t'] <= t1]
    if len(inside) < 2:
        return None
    a, b = inside[0], inside[-1]
    return (b[key] - a[key]) * 1000 / (b['t'] - a['t'])


def battery_temp():
    m = re.search(r'temperature: (\d+)', sh('dumpsys battery'))
    return int(m.group(1)) / 10 if m else None


def device_ms():
    return int(sh('date +%s%3N').strip())


def run_one(mode, workload, warmup, window):
    sh(f'am force-stop {PKG}')
    adb('logcat', '-c')
    started = sh(f'am start -W -a android.intent.action.VIEW -d rntiexample://bench/{mode}/{workload} {PKG}')
    m = re.search(r'TotalTime: (\d+)', started)
    time.sleep(warmup)
    pid = sh(f'pidof {PKG}').strip()
    sh(f'dumpsys gfxinfo {PKG} reset')
    t0, c0 = device_ms(), cpu_ticks(pid)
    time.sleep(window)
    t1, c1 = device_ms(), cpu_ticks(pid)
    row = {'mode': mode, 'workload': workload, 'start_ms': int(m.group(1)) if m else None,
           'window_s': round((t1 - t0) / 1000, 3),
           'cpu_ms_per_s': round((c1 - c0) * 10 * 1000 / (t1 - t0), 2),  # CLK_TCK=100
           'rss_kb': status_kb(pid, 'VmRSS'), **gfx(), 'pss_kb': pss_kb()}
    if not row['frames']:
        row.update(janky_pct=None, p90_ms=None, p99_ms=None)
    lines = bench_lines()
    for key, col in [('frames', 'js_frames_per_s'), ('spans', 'spans_per_s'),
                     ('nativeCalls', 'native_calls_per_s')]:
        r = rate(lines, key, t0, t1)
        row[col] = round(r, 1) if r is not None else None
    row['battery_temp_c'] = battery_temp()
    sh(f'am force-stop {PKG}')
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reps', type=int, default=8)
    ap.add_argument('--warmup', type=float, default=5)
    ap.add_argument('--window', type=float, default=15)
    ap.add_argument('--rest', type=float, default=3)
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--modes', default=','.join(MODES))
    ap.add_argument('--workloads', default=','.join(WORKLOADS))
    args = ap.parse_args()
    rnd = random.Random(args.seed)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        order = 0
        for rep in range(args.reps):
            # Each rep runs every mode once per workload, shuffled to spread drift and heat.
            cells = [(m, wl) for wl in args.workloads.split(',') for m in args.modes.split(',')]
            rnd.shuffle(cells)
            for mode, workload in cells:
                row = run_one(mode, workload, args.warmup, args.window)
                row.update(rep=rep, order=order)
                order += 1
                w.writerow(row)
                f.flush()
                print(json.dumps(row), flush=True)
                time.sleep(args.rest)


if __name__ == '__main__':
    main()
