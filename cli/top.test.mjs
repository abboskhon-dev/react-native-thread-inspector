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
function session(counts, { anchors = true, stale = 0 } = {}) {
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
    join(dir, 'hermes.cpuprofile'),
    JSON.stringify({ stackFrames, samples })
  );
  if (anchors)
    writeFileSync(
      join(dir, 'session.json'),
      JSON.stringify({
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
