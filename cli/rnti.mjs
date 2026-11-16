#!/usr/bin/env node
import { parseArgs } from 'node:util';
import { mergeIncident } from './merge.mjs';
import { pull } from './pull.mjs';
import {
  diffSessions,
  formatDiff,
  formatSummary,
  summarizeSession,
  toJson,
} from './top.mjs';

const USAGE = `usage:
  rnti pull <android|ios> <app-id> [--device <id>] [--out <dir>]
  rnti top <session-dir> [--top <n>] [--json]
  rnti diff <session-a> <session-b> [--top <n>] [--json]
  rnti merge [--allow-unverified] <incident-dir> <session-dir>

pull copies saved incidents and sampling sessions from a device (adb) or a
booted iOS simulator into --out (default: rnti-data).`;

function usage() {
  console.error(USAGE);
  process.exit(2);
}

function merge([incidentDir, sessionDir], { 'allow-unverified': allow }) {
  if (!incidentDir || !sessionDir) usage();
  const result = mergeIncident(incidentDir, sessionDir, {
    allowUnverified: allow,
  });
  if (!result.provenance.verified)
    console.warn(
      `unverified provenance: ${result.provenance.problems.join('; ')}`
    );
  for (const t of result.triggers) {
    console.log(
      `trigger ${t.trigger_index}: ${t.samples} samples (${t.effective_hz} Hz)`
    );
    const hot = t.hot_path
      .slice(-4)
      .map((f) => f.frame)
      .join(' > ');
    if (hot) console.log(`  hot path: ... > ${hot}`);
    for (const f of t.top_self.slice(0, 3))
      console.log(`  self ${(f.share * 100).toFixed(0)}%  ${f.frame}`);
  }
  console.log(`wrote ${incidentDir}/trace.sampled.json and samples.json`);
}

function runPull([platform, app], { device, out }) {
  if (!platform || !app) usage();
  const r = pull({ platform, app, device, out });
  console.log(
    `${r.incidents.length} incidents and ${r.sessions.length} sessions in ${r.out}`
  );
  if (r.sessions.length) console.log(`latest session: ${r.sessions.at(-1)}`);
}

function top([sessionDir], { top: n, json }) {
  if (!sessionDir) usage();
  const s = summarizeSession(sessionDir, { top: n });
  console.log(json ? toJson(s) : formatSummary(s));
}

function diff([a, b], { top: n, json }) {
  if (!a || !b) usage();
  const d = diffSessions(a, b, { top: n });
  console.log(json ? toJson(d) : formatDiff(d));
}

const commands = { merge, pull: runPull, top, diff };

let parsed;
try {
  parsed = parseArgs({
    allowPositionals: true,
    options: {
      'allow-unverified': { type: 'boolean', default: false },
      'device': { type: 'string' },
      'out': { type: 'string' },
      'top': { type: 'string' },
      'json': { type: 'boolean', default: false },
    },
  });
} catch (e) {
  console.error(e.message);
  usage();
}
const [cmd, ...rest] = parsed.positionals;
if (!commands[cmd]) usage();
const limit = parsed.values.top === undefined ? 15 : Number(parsed.values.top);
if (!Number.isInteger(limit) || limit < 1) usage();
try {
  commands[cmd](rest, { ...parsed.values, top: limit });
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
