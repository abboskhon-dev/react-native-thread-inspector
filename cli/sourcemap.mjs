import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { join } from 'node:path';

const CHAINS = [
  ['metro-symbolicate/package.json'],
  [
    'react-native/package.json',
    '@react-native/community-cli-plugin/package.json',
    'metro/package.json',
    'metro-symbolicate/package.json',
  ],
];
const MIN_NAMED_FRAMES = 5;
const MIN_AGREEMENT = 0.5;

// Uses the Metro symbolicator that React Native already installs in the app; rnti ships no copy.
function loadMetro() {
  for (const root of [join(process.cwd(), 'package.json'), import.meta.url]) {
    for (const chain of CHAINS) {
      try {
        let req = createRequire(root);
        for (const step of chain) req = createRequire(req.resolve(step));
        return {
          Symbolication: req('./src/Symbolication.js'),
          SourceMapConsumer: req('source-map').SourceMapConsumer,
        };
      } catch {
        continue;
      }
    }
  }
  throw new Error(
    '--sourcemap needs metro-symbolicate; run rnti inside a React Native project'
  );
}

// Keeps the package and file name, never the host directory.
export function shortSource(source) {
  const parts = source.split(/[\\/]/).filter(Boolean);
  const nm = parts.lastIndexOf('node_modules');
  if (nm >= 0 && nm < parts.length - 2) {
    const scoped = parts[nm + 1].startsWith('@') && nm < parts.length - 3;
    const pkg = parts.slice(nm + 1, nm + (scoped ? 3 : 2)).join('/');
    return `${pkg}/${parts.at(-1)}`;
  }
  return parts.slice(-2).join('/');
}

const lastSegment = (name) => (name ?? '').split(/[#.]/).at(-1);

const contexts = new Map();

function contextFor(mapPath) {
  if (!contexts.has(mapPath)) {
    const { Symbolication, SourceMapConsumer } = loadMetro();
    contexts.set(
      mapPath,
      Symbolication.createContext(
        SourceMapConsumer,
        readFileSync(mapPath, 'utf8')
      )
    );
  }
  return contexts.get(mapPath);
}

// Hermes release frames carry a bytecode address; the composed source map is keyed on it at line 1.
export function createSymbolicator(mapPath) {
  const context = contextFor(mapPath);
  const stats = { js_frames: 0, mapped: 0, named: 0, names_agree: 0 };
  const cache = new Map();
  const resolve = (frame) => {
    if (frame.funcVirtAddr == null || frame.offset == null) return null;
    stats.js_frames++;
    const addr = parseInt(frame.funcVirtAddr, 10);
    const at = context.getOriginalPositionDetailsFor(
      1,
      addr + parseInt(frame.offset, 10)
    );
    const named = Boolean(frame.name) && !frame.name.startsWith('?');
    if (named) stats.named++;
    if (at.source == null) return null;
    stats.mapped++;
    const entry = context.getOriginalPositionDetailsFor(1, addr);
    if (
      named &&
      [at.functionName, entry.functionName, entry.name].some(
        (n) => lastSegment(n) === frame.name
      )
    )
      stats.names_agree++;
    // The function at the sampled address, which after inlining can differ from the bytecode function.
    const name =
      at.functionName || entry.functionName || entry.name || frame.name;
    const shown =
      !name || name === '<anonymous>' ? `(anonymous @${addr})` : name;
    return `${shown} (${shortSource(at.source)})`;
  };
  const symbolicate = (frame) => {
    if (!cache.has(frame)) cache.set(frame, resolve(frame));
    return cache.get(frame);
  };
  symbolicate.stats = stats;
  return symbolicate;
}

// Hermes keeps function names in release bytecode, so they show whether the map belongs to this build.
export function checkSourceMap(symbolicate, { allowUnverified = false } = {}) {
  const { js_frames, mapped, named, names_agree } = symbolicate.stats;
  const agreement =
    named >= MIN_NAMED_FRAMES ? +(names_agree / named).toFixed(3) : null;
  const verified = agreement == null ? null : agreement >= MIN_AGREEMENT;
  if (verified === false && !allowUnverified)
    throw new Error(
      `source map does not match this profile: only ${(agreement * 100).toFixed(0)}% of ${named} function names agree. It must be the composed Hermes map of the same build; pass --allow-unverified to use it anyway`
    );
  return {
    js_frames,
    mapped,
    name_agreement: agreement,
    verified,
  };
}
