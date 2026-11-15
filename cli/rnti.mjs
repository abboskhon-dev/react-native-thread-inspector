#!/usr/bin/env node
import { mergeIncident } from './merge.mjs';

const args = process.argv.slice(2);
const allowUnverified = args.includes('--allow-unverified');
const [cmd, incidentDir, sessionDir] = args.filter((a) => !a.startsWith('--'));
if (cmd !== 'merge' || !incidentDir || !sessionDir) {
  console.error(
    'usage: rnti merge [--allow-unverified] <incident-dir> <session-dir>'
  );
  process.exit(2);
}
let result;
try {
  result = mergeIncident(incidentDir, sessionDir, { allowUnverified });
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
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
