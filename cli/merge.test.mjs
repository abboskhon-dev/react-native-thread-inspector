import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import {
  frameName,
  hotPath,
  loadSamples,
  mergeIncident,
  sampleSlices,
} from './merge.mjs';

const MS = 1_000_000;

function profile() {
  const stackFrames = {
    1: { name: '[root]', category: 'root' },
    2: {
      name: 'run(/Users/someone/app/main.jsbundle:1:2)',
      category: 'JavaScript',
      parent: 1,
    },
    3: { name: 'parse', category: 'JavaScript', parent: 2 },
    4: { name: '[Native] dateNow', category: 'Native', parent: 3 },
  };
  const samples = [];
  for (let i = 0; i < 100; i++) {
    const sf = i < 10 || i >= 90 ? 1 : i % 3 === 0 ? 4 : 3;
    samples.push({ ts: String((1000 + i) * 1000), tid: '7', sf });
  }
  return { traceEvents: [], stackFrames, samples };
}

test('frame names drop host paths', () => {
  assert.equal(
    frameName('run(/Users/someone/app/main.jsbundle:1:2)'),
    'run (main.jsbundle:1:2)'
  );
  assert.equal(frameName('(/a/b/main.jsbundle:3:4)'), '(main.jsbundle:3:4)');
  assert.equal(frameName(''), '(anonymous)');
});

test('slices merge consecutive samples and stop at idle', () => {
  const slices = sampleSlices(loadSamples(profile()));
  const parse = slices.filter((s) => s.name === 'parse');
  assert.equal(parse.length, 1);
  assert.equal(parse[0].startNs, 1010 * MS);
  assert.equal(parse[0].endNs, 1090 * MS);
  assert.equal(parse[0].depth, 1);
  assert.ok(slices.filter((s) => s.name.startsWith('[Native]')).length > 1);
});

test('hot path stops where samples diverge', () => {
  const path = hotPath([
    ['a', 'b', 'c'],
    ['a', 'b', 'd'],
    ['a', 'b', 'c'],
    ['a', 'x'],
  ]);
  assert.deepEqual(
    path.map((f) => f.frame),
    ['a', 'b', 'c']
  );
});

test('merge adds a sample track and per-trigger summary', () => {
  const inc = mkdtempSync(join(tmpdir(), 'rnti-inc-'));
  const ses = mkdtempSync(join(tmpdir(), 'rnti-ses-'));
  writeFileSync(join(ses, 'hermes.cpuprofile'), JSON.stringify(profile()));
  writeFileSync(
    join(inc, 'trace.json'),
    JSON.stringify({ traceEvents: [{ ph: 'M', pid: 1, name: 'process_name' }] })
  );
  writeFileSync(
    join(inc, 'incident.json'),
    JSON.stringify({
      id: 1,
      window: { start_ns: 900 * MS, capture_end_ns: 1200 * MS },
      triggers: [{ tid: 7, onset_ns: 1020 * MS, end_ns: 1080 * MS }],
    })
  );
  const r = mergeIncident(inc, ses);
  assert.equal(r.triggers[0].samples, 60);
  assert.equal(r.triggers[0].top_inclusive[0].share, 1);
  assert.deepEqual(
    r.triggers[0].hot_path.map((f) => f.frame),
    ['run (main.jsbundle:1:2)', 'parse']
  );
  const trace = JSON.parse(
    readFileSync(join(inc, 'trace.sampled.json'), 'utf8')
  );
  assert.ok(
    trace.traceEvents.some(
      (e) => e.cat === 'rnti.hermes_sample' && e.name === 'parse'
    )
  );
  assert.ok(!JSON.stringify(trace).includes('/Users/'));
});
