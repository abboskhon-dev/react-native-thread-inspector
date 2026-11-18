import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import {
  diffSessions,
  formatDiff,
  formatSummary,
  summarizeSession,
  toJson,
} from './top.mjs';

const MS = 1_000_000;

// counts: samples per leaf frame, in order; 'idle' is a sample with no JS stack.
function session(counts, { anchors = true, stale = 0, cpu } = {}) {
  const stackFrames = {
    1: { name: '[root]', category: 'root' },
    2: { name: 'run', category: 'JavaScript', parent: 1 },
    3: { name: 'parse', category: 'JavaScript', parent: 2 },
    4: { name: 'render', category: 'JavaScript', parent: 2 },
    5: { name: '', funcVirtAddr: '77', category: 'JavaScript', parent: 2 },
    6: { name: '', funcVirtAddr: '88', category: 'JavaScript', parent: 2 },
  };
  const sf = { idle: 1, parse: 3, render: 4, anonA: 5, anonB: 6 };
  const samples = [];
  for (let i = 0; i < stale; i++)
    samples.push({ ts: String((500 + i) * 1000), tid: '7', sf: 3 });
  let t = 1000;
  for (const [name, n] of Object.entries(counts))
    for (let i = 0; i < n; i++)
      samples.push({ ts: String(t++ * 1000), tid: '7', sf: sf[name] });
  const dir = mkdtempSync(join(tmpdir(), 'rnti-top-'));
  writeFileSync(
    join(dir, anchors ? 'hermes.trace.json' : 'hermes.cpuprofile'),
    JSON.stringify({ stackFrames, samples })
  );
  if (anchors)
    writeFileSync(
      join(dir, 'session.json'),
      JSON.stringify({
        profile: 'hermes.trace.json',
        js_tid: 7,
        cpu,
        start: { mono_ns: 1000 * MS },
        stop: { mono_ns: 1100 * MS },
      })
    );
  return dir;
}

test('summary ranks self time and reports how busy the thread was', () => {
  const s = summarizeSession(session({ idle: 60, parse: 30, render: 10 }));
  const [t] = s.threads;
  assert.equal(s.duration_ms, 100);
  assert.equal(t.samples, 100);
  assert.equal(t.busy_share, 0.4);
  assert.equal(t.effective_hz, 1000);
  assert.deepEqual(
    t.top_self.map((f) => [f.frame, f.share]),
    [
      ['parse', 0.75],
      ['render', 0.25],
    ]
  );
  assert.equal(t.top_inclusive[0].frame, 'run');
  assert.match(formatSummary(s), /75\.0% {2}75\.0% parse/);
  assert.match(formatSummary(s), /tid 7 \(JS thread\)/);
  assert.ok(!toJson(s).includes('self_counts'));
});

test('samples from before the session start are dropped', () => {
  const s = summarizeSession(session({ parse: 10 }, { stale: 3 }));
  assert.equal(s.samples, 10);
  assert.equal(s.samples_outside_session, 3);
  assert.match(formatSummary(s), /dropped 3 samples/);
});

test('a profile without session.json is bounded by its samples', () => {
  const s = summarizeSession(
    session({ parse: 11 }, { anchors: false, stale: 2 })
  );
  assert.equal(s.bounds, 'first and last sample');
  assert.equal(s.samples, 13);
});

test('a directory without a profile fails with a clear message', () => {
  assert.throws(
    () => summarizeSession(mkdtempSync(join(tmpdir(), 'rnti-empty-'))),
    /no Hermes profile/
  );
});

test('unnamed functions are kept apart by address', () => {
  const s = summarizeSession(session({ anonA: 3, anonB: 1 }));
  assert.deepEqual(
    s.threads[0].top_self.map((f) => f.frame),
    ['(anonymous @77)', '(anonymous @88)']
  );
});

test('diff reports the change in self share between two sessions', () => {
  const a = session({ idle: 50, parse: 40, render: 10 });
  const b = session({ idle: 20, parse: 20, render: 60 });
  const d = diffSessions(a, b);
  assert.equal(d.a.busy_share, 0.5);
  assert.equal(d.b.busy_share, 0.8);
  assert.deepEqual(d.changes, [
    { frame: 'parse', share_a: 0.8, share_b: 0.25, delta: -0.55 },
    { frame: 'render', share_a: 0.2, share_b: 0.75, delta: 0.55 },
  ]);
  assert.match(formatDiff(d), /\+55\.0\s+render/);
});

test('per-thread CPU is listed when the session recorded it', () => {
  const cpu = {
    wall_ns: 2000 * MS,
    process_ns: 1500 * MS,
    threads: [
      { tid: 9, name: 'RenderThread', cpu_ns: 1000 * MS },
      { tid: 7, name: 'mqt_v_js', cpu_ns: 400 * MS },
      { tid: 11, name: '', cpu_ns: 50 * MS },
    ],
  };
  const s = summarizeSession(session({ parse: 10 }, { cpu }), { top: 2 });
  assert.equal(s.cpu.process_core_share, 0.75);
  assert.equal(s.cpu.threads_with_cpu, 3);
  assert.deepEqual(
    s.cpu.threads.map((t) => [t.name, t.cpu_ms, t.core_share]),
    [
      ['RenderThread', 1000, 0.5],
      ['mqt_v_js', 400, 0.2],
    ]
  );
  const text = formatSummary(s);
  assert.match(text, /process 1500 ms \(75\.0% of one core\), 3 threads/);
  assert.match(text, /400\.0 {2}20\.0% {2}mqt_v_js \(tid 7, JS thread\)/);
  assert.doesNotMatch(text, /vol\/s/);
  assert.equal(summarizeSession(session({ parse: 10 })).cpu, null);
});

test('context switches per second are listed when the session recorded them', () => {
  const cpu = {
    wall_ns: 2000 * MS,
    process_ns: 600 * MS,
    threads: [
      { tid: 7, name: 'mqt_v_js', cpu_ns: 400 * MS, voluntary_switches: 10, involuntary_switches: 40 },
      { tid: 12, name: 'pool-1', cpu_ns: 200 * MS, voluntary_switches: 1800, involuntary_switches: 6 },
    ],
  };
  const s = summarizeSession(session({ parse: 10 }, { cpu }));
  assert.deepEqual(
    s.cpu.threads.map((t) => [t.name, t.voluntary_per_s, t.involuntary_per_s]),
    [
      ['mqt_v_js', 5, 20],
      ['pool-1', 900, 3],
    ]
  );
  const text = formatSummary(s);
  assert.match(text, /cpu ms {3}core {2}vol\/s {2}invol\/s {2}thread/);
  assert.match(text, /200\.0 {2}10\.0% {4}900 {8}3 {2}pool-1/);
});
