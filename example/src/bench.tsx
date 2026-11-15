import { useEffect, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import * as TI from 'react-native-thread-inspector';

// none: no spans, same native calls; off: spans but recorder never started; record: core without probing.
export type BenchMode =
  'none' | 'off' | 'record' | 'core' | 'full' | 'sampling';
export type BenchWorkload = 'idle' | 'active';
export type BenchConfig = { mode: BenchMode; workload: BenchWorkload };

const MODES: BenchMode[] = [
  'none',
  'off',
  'record',
  'core',
  'full',
  'sampling',
];
const WORKLOADS: BenchWorkload[] = ['idle', 'active'];
const CELLS = 300;
const SPANS_PER_FRAME = 10;
const NATIVE_CALLS_PER_FRAME = 10;

// rntiexample://bench/<mode>/<workload>
export function parseBenchUrl(url?: string | null): BenchConfig | null {
  const m = url?.match(/bench\/(\w+)\/(\w+)/);
  if (!m) return null;
  const mode = m[1] as BenchMode;
  const workload = m[2] as BenchWorkload;
  return MODES.includes(mode) && WORKLOADS.includes(workload)
    ? { mode, workload }
    : null;
}

function startMode(mode: BenchMode) {
  if (mode === 'none' || mode === 'off') return;
  const adapters = mode !== 'core' && mode !== 'record';
  TI.start({
    turboModules: adapters,
    fabric: adapters,
    ...(mode === 'record' && { probeIntervalMs: 3_600_000 }),
  });
  if (mode === 'sampling') TI.sampling.start(100);
}

function smallWork(i: number) {
  let x = i;
  for (let k = 0; k < 200; k++) x = (x * 31 + k) % 1_000_003;
  return x;
}

export function BenchScreen({ config }: { config: BenchConfig }) {
  const [tick, setTick] = useState(0);

  useEffect(() => {
    startMode(config.mode);
    const counters = { frames: 0, spans: 0, nativeCalls: 0 };
    let raf = 0;
    const bare = config.mode === 'none';
    const frame = () => {
      counters.frames++;
      for (let i = 0; i < SPANS_PER_FRAME; i++) {
        if (bare) smallWork(i);
        else TI.measureSync('bench.work', () => smallWork(i));
        counters.spans++;
      }
      for (let i = 0; i < NATIVE_CALLS_PER_FRAME; i++) {
        TI.debug.simulateNativeBlock(0);
        counters.nativeCalls++;
      }
      setTick((t) => t + 1);
      raf = requestAnimationFrame(frame);
    };
    if (config.workload === 'active') raf = requestAnimationFrame(frame);
    // Host script reads these from logcat; cumulative so missed lines don't matter.
    const log = setInterval(() => {
      console.log(
        `RNTI_BENCH ${JSON.stringify({ t: Date.now(), ...counters })}`
      );
    }, 1000);
    return () => {
      cancelAnimationFrame(raf);
      clearInterval(log);
    };
  }, [config]);

  return (
    <View style={styles.root}>
      <Text style={styles.label}>
        bench {config.mode}/{config.workload}
      </Text>
      <View style={styles.grid}>
        {Array.from({ length: CELLS }, (_, i) => (
          <View
            key={i}
            style={[styles.cell, (i + tick) % 5 === 0 && styles.cellOn]}
          />
        ))}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, paddingTop: 60, backgroundColor: '#fff' },
  label: { fontFamily: 'monospace', margin: 8 },
  grid: { flexDirection: 'row', flexWrap: 'wrap' },
  cell: { width: 14, height: 14, margin: 2, backgroundColor: '#cde' },
  cellOn: { backgroundColor: '#357' },
});
