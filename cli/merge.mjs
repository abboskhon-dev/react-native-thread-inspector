import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const SAMPLE_TID_OFFSET = 0x7e000000;

// Strips absolute bundle paths so merged files carry no host directories.
export function frameName(raw) {
  const name = raw || '(anonymous)';
  return name
    .replace(/\(([^()]*\/)?([^/()]+:\d+:\d+)\)$/, (_, _dir, loc) => ` (${loc})`)
    .trim();
}

export function loadSamples(profile) {
  const frames = profile.stackFrames ?? {};
  const stackCache = new Map();
  const stackOf = (id) => {
    if (stackCache.has(id)) return stackCache.get(id);
    const out = [];
    let cur = id;
    while (cur != null && frames[String(cur)]) {
      const f = frames[String(cur)];
      if (f.category !== 'root') out.push(frameName(f.name));
      cur = f.parent;
    }
    out.reverse();
    stackCache.set(id, out);
    return out;
  };
  return (profile.samples ?? [])
    .map((s) => ({
      tsNs: Math.round(Number(s.ts) * 1000),
      tid: Number(s.tid),
      stack: stackOf(s.sf),
    }))
    .sort((a, b) => a.tsNs - b.tsNs);
}

function median(xs) {
  if (!xs.length) return 0;
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.floor(s.length / 2)];
}

// A sample covers one sampling period; longer gaps and the time after the last sample stay empty.
function withDurations(samples) {
  const byTid = new Map();
  for (const s of samples) {
    if (!byTid.has(s.tid)) byTid.set(s.tid, []);
    byTid.get(s.tid).push(s);
  }
  const out = [];
  for (const list of byTid.values()) {
    const gaps = list.slice(1).map((s, i) => s.tsNs - list[i].tsNs);
    const period = median(gaps);
    list.forEach((s, i) => {
      const gap = list[i + 1] ? list[i + 1].tsNs - s.tsNs : Infinity;
      out.push({ ...s, durNs: gap <= 3 * period ? gap : period });
    });
  }
  return out;
}

export function sampleSlices(samples) {
  const slices = [];
  const byTid = new Map();
  for (const s of withDurations(samples)) {
    if (!byTid.has(s.tid)) byTid.set(s.tid, []);
    byTid.get(s.tid).push(s);
  }
  for (const [tid, list] of byTid) {
    let open = [];
    let lastEnd = 0;
    const close = (depth, endNs) => {
      while (open.length > depth) {
        const o = open.pop();
        slices.push({
          tid,
          name: o.name,
          startNs: o.startNs,
          endNs,
          depth: open.length,
        });
      }
    };
    for (const s of list) {
      if (s.tsNs > lastEnd) close(0, lastEnd);
      let d = 0;
      while (
        d < open.length &&
        d < s.stack.length &&
        open[d].name === s.stack[d]
      )
        d++;
      close(d, s.tsNs);
      for (let i = d; i < s.stack.length; i++)
        open.push({ name: s.stack[i], startNs: s.tsNs });
      lastEnd = s.tsNs + s.durNs;
    }
    close(0, lastEnd);
  }
  return slices;
}

// Deepest common call path still covering at least half of the busy samples.
export function hotPath(stacks, minShare = 0.5) {
  const path = [];
  let cur = stacks;
  const need = Math.ceil(stacks.length * minShare);
  for (let d = 0; cur.length >= need && cur.length > 0; d++) {
    const counts = new Map();
    for (const st of cur)
      if (st[d] !== undefined) counts.set(st[d], (counts.get(st[d]) ?? 0) + 1);
    const best = [...counts.entries()].sort((a, b) => b[1] - a[1])[0];
    if (!best || best[1] < need) break;
    path.push({ frame: best[0], share: +(best[1] / stacks.length).toFixed(3) });
    cur = cur.filter((st) => st[d] === best[0]);
  }
  return path;
}

export function summarize(samples, startNs, endNs, tid, top = 10) {
  const inside = withDurations(samples).filter(
    (s) => s.tsNs >= startNs && s.tsNs < endNs && (!tid || s.tid === tid)
  );
  const self = new Map();
  const total = new Map();
  let busy = 0;
  for (const s of inside) {
    if (!s.stack.length) continue;
    busy++;
    const leaf = s.stack[s.stack.length - 1];
    self.set(leaf, (self.get(leaf) ?? 0) + 1);
    for (const f of new Set(s.stack)) total.set(f, (total.get(f) ?? 0) + 1);
  }
  const rank = (m) =>
    [...m.entries()]
      .sort((a, b) => b[1] - a[1])
      .slice(0, top)
      .map(([frame, n]) => ({
        frame,
        samples: n,
        share: busy ? +(n / busy).toFixed(3) : 0,
      }));
  const span = endNs - startNs;
  return {
    hot_path: hotPath(inside.filter((s) => s.stack.length).map((s) => s.stack)),
    samples: inside.length,
    samples_with_js_stack: busy,
    effective_hz: span > 0 ? +((inside.length * 1e9) / span).toFixed(1) : 0,
    top_self: rank(self),
    top_inclusive: rank(total),
  };
}

// Refuses profiles from another recorder session, process or clock unless explicitly allowed.
export function checkProvenance(report, pid, session) {
  const checks = {
    recorder_session_id: [session?.recorder_session_id, report.session_id],
    pid: [session?.pid, pid],
    clock_domain: [session?.clock_domain, report.clock?.domain],
  };
  const problems = Object.entries(checks)
    .filter(([, [a, b]]) => a == null || b == null || a !== b)
    .map(
      ([k, [a, b]]) =>
        `${k}: session ${a ?? 'missing'} vs incident ${b ?? 'missing'}`
    );
  return { verified: problems.length === 0, problems };
}

export function mergeIncident(
  incidentDir,
  sessionDir,
  { allowUnverified = false } = {}
) {
  const trace = JSON.parse(
    readFileSync(join(incidentDir, 'trace.json'), 'utf8')
  );
  const report = JSON.parse(
    readFileSync(join(incidentDir, 'incident.json'), 'utf8')
  );
  const profile = JSON.parse(
    readFileSync(join(sessionDir, 'hermes.cpuprofile'), 'utf8')
  );
  const sessionPath = join(sessionDir, 'session.json');
  const session = existsSync(sessionPath)
    ? JSON.parse(readFileSync(sessionPath, 'utf8'))
    : null;
  const pid = trace.traceEvents.find((e) => e.pid != null)?.pid ?? 0;
  const provenance = checkProvenance(report, pid, session);
  if (!provenance.verified && !allowUnverified) {
    throw new Error(
      `profile does not match incident (${provenance.problems.join('; ')}); pass --allow-unverified to merge anyway`
    );
  }

  const samples = loadSamples(profile);
  const winStart = Math.max(
    report.window.start_ns,
    session?.start?.mono_ns ?? 0
  );
  const winEnd = Math.min(
    report.window.capture_end_ns,
    session?.stop?.mono_ns ?? Infinity
  );
  const tids = new Set();
  for (const raw of sampleSlices(samples)) {
    const sl = {
      ...raw,
      startNs: Math.max(raw.startNs, winStart),
      endNs: Math.min(raw.endNs, winEnd),
    };
    if (sl.endNs <= sl.startNs) continue;
    const tid = SAMPLE_TID_OFFSET + (sl.tid % 0x1000000);
    tids.add(tid);
    trace.traceEvents.push({
      ph: 'X',
      cat: 'rnti.hermes_sample',
      name: sl.name,
      pid,
      tid,
      ts: sl.startNs / 1000,
      dur: (sl.endNs - sl.startNs) / 1000,
      args: { sampled: true, reconstructed_from_samples: true },
    });
  }
  for (const tid of tids) {
    trace.traceEvents.push({
      ph: 'M',
      pid,
      tid,
      name: 'thread_name',
      args: { name: `hermes samples (tid ${tid - SAMPLE_TID_OFFSET})` },
    });
  }

  const sampled = samples.filter((s) => s.tsNs >= winStart && s.tsNs < winEnd);
  const result = {
    schema: 'rnti.samples/1',
    incident_id: report.id,
    measurement:
      'statistical JS stack samples; slice durations are inferred from sample spacing',
    provenance,
    coverage: {
      clipped_to_ns: [winStart, winEnd],
      samples_in_window: sampled.length,
      first_sample_ns: samples[0]?.tsNs ?? null,
      last_sample_ns: samples.at(-1)?.tsNs ?? null,
    },
    triggers: report.triggers.map((t, i) => {
      const end = t.end_ns ?? winEnd;
      return {
        trigger_index: i,
        tid: t.tid,
        ...summarize(
          samples,
          Math.max(t.onset_ns, winStart),
          Math.min(end, winEnd),
          t.tid
        ),
      };
    }),
  };
  writeFileSync(join(incidentDir, 'trace.sampled.json'), JSON.stringify(trace));
  writeFileSync(
    join(incidentDir, 'samples.json'),
    JSON.stringify(result, null, 2)
  );
  return result;
}
