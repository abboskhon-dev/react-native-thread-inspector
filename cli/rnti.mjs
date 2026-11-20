#!/usr/bin/env node
import { parseArgs } from 'node:util';
import { mergeIncident } from './merge.mjs';
import { pull } from './pull.mjs';
import { record } from './record.mjs';
import {
  diffSessions,
  formatDiff,
  formatSummary,
  summarizeSession,
  toJson,
} from './top.mjs';

const USAGE = `usage:
  rnti record <start|stop> <android|ios> <app-id> [--device <id>]
  rnti pull <android|ios> <app-id> [--device <id>] [--out <dir>]
  rnti top <session-dir> [--top <n>] [--json] [--sourcemap <file>]
  rnti diff <session-a> <session-b> [--top <n>] [--json] [--sourcemap <file>]
  rnti merge <incident-dir> [<session-dir>] [--sourcemap <file>]

--sourcemap takes the composed Hermes map of the same build (Android:
android/app/build/generated/sourcemaps/react/release/index.android.bundle.map).
--allow-unverified accepts a profile or source map that fails its match check.

record starts or stops Hermes sampling in a running app that called
start({ remoteSampling: true }); Android over adb, iOS on a booted simulator.
pull copies saved incidents and sampling sessions from an Android device (adb),
an iOS simulator, or an iPhone (devicectl) into --out (default: rnti-data).`;

function usage() {
  console.error(USAGE);
  process.exit(2);
}

function merge([incidentDir, sessionDir], options) {
  if (!incidentDir) usage();
  const result = mergeIncident(incidentDir, sessionDir, options);
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

async function runRecord([cmd, platform, app], { device }) {
  if (!cmd || !platform || !app) usage();
  const r = await record({ cmd, platform, app, device });
  console.log(JSON.stringify(r));
  if (r.status !== 'available') process.exitCode = 1;
}

function top([sessionDir], options) {
  if (!sessionDir) usage();
  const s = summarizeSession(sessionDir, options);
  console.log(options.json ? toJson(s) : formatSummary(s));
}

function diff([a, b], options) {
  if (!a || !b) usage();
  const d = diffSessions(a, b, options);
  console.log(options.json ? toJson(d) : formatDiff(d));
}

const commands = { merge, pull: runPull, record: runRecord, top, diff };

let parsed;
try {
  parsed = parseArgs({
    allowPositionals: true,
    options: {
      'allow-unverified': { type: 'boolean', default: false },
      'device': { type: 'string' },
      'out': { type: 'string' },
      'sourcemap': { type: 'string' },
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
  await commands[cmd](rest, {
    ...parsed.values,
    top: limit,
    sourceMap: parsed.values.sourcemap,
    allowUnverified: parsed.values['allow-unverified'],
  });
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
