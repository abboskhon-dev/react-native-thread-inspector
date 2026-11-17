import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { shortSource } from './sourcemap.mjs';
import { summarizeSession, toJson } from './top.mjs';

const metro = createRequire(
  createRequire(import.meta.url).resolve('metro-symbolicate/package.json')
);
const { SourceMapGenerator } = metro('source-map');

// Bytecode address -> where that function lives in the sources.
const FUNCTIONS = [
  [100, 'parseFeed', '/Users/someone/app/src/feed.ts'],
  [200, 'render', '/Users/someone/app/node_modules/@scope/ui/lib/List.js'],
  [300, null, '/Users/someone/app/src/feed.ts'],
  [400, 'sortItems', '/Users/someone/app/src/feed.ts'],
  [500, 'formatDate', '/Users/someone/app/src/dates.ts'],
  [600, 'useFeed', '/Users/someone/app/src/hooks/useFeed.ts'],
];

function sourceMap() {
  const gen = new SourceMapGenerator();
  FUNCTIONS.forEach(([addr, name, source], i) =>
    gen.addMapping({
      generated: { line: 1, column: addr },
      original: { line: 10 * (i + 1), column: 0 },
      source,
      ...(name && { name }),
    })
  );
  const path = join(mkdtempSync(join(tmpdir(), 'rnti-map-')), 'bundle.map');
  writeFileSync(path, gen.toString());
  return path;
}

// One sample per function, each called from the first; `names` are what Hermes kept in the bytecode.
function session(names) {
  const stackFrames = { 1: { name: '[root]', category: 'root' } };
  const samples = [];
  FUNCTIONS.forEach(([addr], i) => {
    stackFrames[i + 2] = {
      name: names[i] ?? '',
      funcVirtAddr: String(addr),
      offset: '4',
      category: 'JavaScript',
      parent: i === 0 ? 1 : 2,
    };
    samples.push({ ts: String((1000 + i) * 1000), tid: '7', sf: i + 2 });
  });
  const dir = mkdtempSync(join(tmpdir(), 'rnti-sm-'));
  writeFileSync(
    join(dir, 'hermes.trace.json'),
    JSON.stringify({ stackFrames, samples })
  );
  return dir;
}

const HERMES_NAMES = FUNCTIONS.map(([, name]) => name);

test('frames are named from the source map without host paths', () => {
  const s = summarizeSession(session(HERMES_NAMES), { sourceMap: sourceMap() });
  assert.deepEqual(s.source_map, {
    js_frames: 6,
    mapped: 6,
    name_agreement: 1,
    verified: true,
  });
  const frames = s.threads[0].top_self.map((f) => f.frame);
  assert.ok(frames.includes('render (@scope/ui/List.js)'));
  assert.ok(frames.includes('formatDate (src/dates.ts)'));
  assert.ok(frames.includes('(anonymous @300) (src/feed.ts)'));
  assert.equal(s.threads[0].top_inclusive[0].frame, 'parseFeed (src/feed.ts)');
  assert.ok(!toJson(s).includes('/Users/'));
});

test('a source map from another build is refused', () => {
  const dir = session(['a', 'b', null, 'c', 'd', 'e']);
  const map = sourceMap();
  assert.throws(
    () => summarizeSession(dir, { sourceMap: map }),
    /does not match this profile: only 0% of 5 function names agree/
  );
  const s = summarizeSession(dir, { sourceMap: map, allowUnverified: true });
  assert.equal(s.source_map.verified, false);
});

test('too few named frames leaves the match unchecked', () => {
  const s = summarizeSession(session(['parseFeed']), {
    sourceMap: sourceMap(),
  });
  assert.equal(s.source_map.verified, null);
});

test('source paths keep the package and file only', () => {
  assert.equal(shortSource('/a/b/app/src/App.tsx'), 'src/App.tsx');
  assert.equal(
    shortSource('/a/node_modules/react-native/Libraries/Core/x.js'),
    'react-native/x.js'
  );
  assert.equal(
    shortSource('C:\\app\\node_modules\\@scope\\ui\\lib\\List.js'),
    '@scope/ui/List.js'
  );
  assert.equal(shortSource('index.js'), 'index.js');
});
