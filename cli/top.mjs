import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { hotPath, loadSamples } from './merge.mjs';

function readSession(sessionDir) {
  const profile = JSON.parse(
    readFileSync(join(sessionDir, 'hermes.cpuprofile'), 'utf8')
  );
  const sessionPath = join(sessionDir, 'session.json');
  const session = existsSync(sessionPath)
    ? JSON.parse(readFileSync(sessionPath, 'utf8'))
    : null;
  return { profile, session };
}

function rank(counts, busy, top) {
  return [...counts.entries()]
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .slice(0, top)
    .map(([frame, n]) => ({
      frame,
      samples: n,
      share: busy ? +(n / busy).toFixed(3) : 0,
    }));
}

function summarizeThread(tid, samples, spanNs, top) {
  const self = new Map();
  const total = new Map();
  const stacks = [];
  for (const s of samples) {
    if (!s.stack.length) continue;
    stacks.push(s.stack);
    const leaf = s.stack[s.stack.length - 1];
    self.set(leaf, (self.get(leaf) ?? 0) + 1);
    for (const f of new Set(s.stack)) total.set(f, (total.get(f) ?? 0) + 1);
  }
  const busy = stacks.length;
  return {
    tid,
    samples: samples.length,
    samples_with_js_stack: busy,
    busy_share: samples.length ? +(busy / samples.length).toFixed(3) : 0,
    effective_hz:
      spanNs > 0 ? +((samples.length * 1e9) / spanNs).toFixed(1) : 0,
    hot_path: hotPath(stacks),
    top_self: rank(self, busy, top).map((f) => ({
      ...f,
      inclusive_share: +(total.get(f.frame) / busy).toFixed(3),
    })),
    top_inclusive: rank(total, busy, top),
    self_counts: self,
  };
}

// Samples outside the session anchors belong to an earlier session and are dropped.
export function summarizeSession(sessionDir, { top = 15 } = {}) {
  const { profile, session } = readSession(sessionDir);
  const all = loadSamples(profile);
  const startNs = session?.start?.mono_ns ?? null;
  const stopNs = session?.stop?.mono_ns ?? null;
  const samples = all.filter(
    (s) =>
      (startNs == null || s.tsNs >= startNs) &&
      (stopNs == null || s.tsNs <= stopNs)
  );
  const fromNs = startNs ?? samples[0]?.tsNs ?? 0;
  const toNs = stopNs ?? samples.at(-1)?.tsNs ?? 0;
  const byTid = new Map();
  for (const s of samples) {
    if (!byTid.has(s.tid)) byTid.set(s.tid, []);
    byTid.get(s.tid).push(s);
  }
  return {
    schema: 'rnti.session_summary/1',
    measurement:
      'statistical JS stack samples; shares are of samples that had a JS stack',
    bounds: session ? 'session anchors' : 'first and last sample',
    duration_ms: +((toNs - fromNs) / 1e6).toFixed(1),
    samples: samples.length,
    samples_outside_session: all.length - samples.length,
    threads: [...byTid.entries()]
      .map(([tid, list]) => summarizeThread(tid, list, toNs - fromNs, top))
      .sort((a, b) => b.samples - a.samples),
  };
}

// Compares the most-sampled thread of each session by share of busy samples.
export function diffSessions(dirA, dirB, { top = 15 } = {}) {
  const [a, b] = [dirA, dirB].map((d) => summarizeSession(d, { top }));
  const [ta, tb] = [a.threads[0], b.threads[0]];
  const share = (t, frame) =>
    t?.samples_with_js_stack
      ? (t.self_counts.get(frame) ?? 0) / t.samples_with_js_stack
      : 0;
  const frames = new Set([
    ...(ta?.self_counts.keys() ?? []),
    ...(tb?.self_counts.keys() ?? []),
  ]);
  const changes = [...frames]
    .map((frame) => ({
      frame,
      share_a: +share(ta, frame).toFixed(3),
      share_b: +share(tb, frame).toFixed(3),
      delta: +(share(tb, frame) - share(ta, frame)).toFixed(3),
    }))
    .filter((c) => c.delta !== 0)
    .sort(
      (x, y) =>
        Math.abs(y.delta) - Math.abs(x.delta) || x.frame.localeCompare(y.frame)
    )
    .slice(0, top);
  const side = (s, t) => ({
    duration_ms: s.duration_ms,
    tid: t?.tid ?? null,
    samples: t?.samples ?? 0,
    busy_share: t?.busy_share ?? 0,
  });
  return {
    schema: 'rnti.session_diff/1',
    measurement:
      'self-time share of busy samples, b minus a; frames match by name, so compare the same build',
    a: side(a, ta),
    b: side(b, tb),
    changes,
  };
}

// Maps drop out of JSON on their own; this keeps the output schema explicit.
export function toJson(summary) {
  return JSON.stringify(
    summary,
    (key, value) => (key === 'self_counts' ? undefined : value),
    2
  );
}

const pct = (x) => `${(x * 100).toFixed(1)}%`.padStart(6);

export function formatSummary(s) {
  const lines = [`${s.samples} samples over ${s.duration_ms} ms (${s.bounds})`];
  if (s.samples_outside_session)
    lines.push(
      `dropped ${s.samples_outside_session} samples outside the session`
    );
  for (const t of s.threads) {
    lines.push(
      '',
      `tid ${t.tid}: ${t.samples} samples, ${t.effective_hz} Hz, ${pct(t.busy_share).trim()} with a JS stack`
    );
    const hot = t.hot_path.map((f) => f.frame).join(' > ');
    if (hot) lines.push(`  hot path: ${hot}`);
    lines.push('  self   incl   frame');
    for (const f of t.top_self)
      lines.push(`  ${pct(f.share)} ${pct(f.inclusive_share)} ${f.frame}`);
  }
  return lines.join('\n');
}

export function formatDiff(d) {
  const lines = [
    `a: ${d.a.samples} samples over ${d.a.duration_ms} ms, ${pct(d.a.busy_share).trim()} with a JS stack`,
    `b: ${d.b.samples} samples over ${d.b.duration_ms} ms, ${pct(d.b.busy_share).trim()} with a JS stack`,
    '',
    '       a      b  delta  frame (self share)',
  ];
  for (const c of d.changes)
    lines.push(
      `  ${pct(c.share_a)} ${pct(c.share_b)} ${(c.delta > 0 ? '+' : '') + (c.delta * 100).toFixed(1)}`.padEnd(
        24
      ) + ` ${c.frame}`
    );
  return lines.join('\n');
}
