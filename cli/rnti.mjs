#!/usr/bin/env node
import { mergeIncident } from './merge.mjs';

const [cmd, incidentDir, sessionDir] = process.argv.slice(2);
if (cmd !== 'merge' || !incidentDir || !sessionDir) {
  console.error('usage: rnti merge <incident-dir> <session-dir>');
  process.exit(2);
}
const result = mergeIncident(incidentDir, sessionDir);
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
