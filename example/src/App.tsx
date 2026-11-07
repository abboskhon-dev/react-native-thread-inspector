import { useEffect, useState } from 'react';
import { Pressable, SafeAreaView, ScrollView, StyleSheet, Text, View } from 'react-native';
import * as TI from 'react-native-thread-inspector';

const capabilities = TI.start({
  historyMs: 5_000,
  incidentThresholdMs: { js: 200, main: 200 },
  probeIntervalMs: 50,
  tailAfterRecoveryMs: 500,
  cooldownMs: 1_500,
});

function busyLoop(ms: number) {
  const end = Date.now() + ms;
  let x = 0;
  while (Date.now() < end) x += Math.sqrt(x + 1);
  return x;
}

const scenarios: { id: string; title: string; run: () => void | Promise<void> }[] = [
  {
    id: 'js-busy',
    title: '1. JS computation (300 ms)',
    run: () => {
      TI.measureSync('demo.parseLargeJson', () => busyLoop(300));
    },
  },
  {
    id: 'native-sync',
    title: '2. Sync native call blocks JS (300 ms)',
    run: () => {
      TI.measureSync('demo.storageRead', () => TI.debug.simulateNativeBlock(300));
    },
  },
  {
    id: 'native-async',
    title: '3. Async native work, JS responsive (300 ms)',
    run: async () => {
      const flow = TI.newFlow();
      const span = TI.begin('demo.asyncRequest', flow);
      await TI.debug.simulateAsyncNativeWork(300, flow);
      TI.end(span);
    },
  },
  {
    id: 'main-block',
    title: '4. Main thread busy (300 ms)',
    run: () => TI.debug.simulateMainThreadBlock(300),
  },
];

type Summary = { path: string; text: string };

function summarize(path: string): Summary {
  try {
    const { report } = TI.readIncident(path);
    const lines: string[] = [`#${report.id} · ${report.window.end_reason}`];
    report.triggers.forEach((t: any, i: number) => {
      const dur = t.duration_ns == null ? 'unrecovered' : `${Math.round(t.duration_ns / 1e6)} ms`;
      lines.push(`${t.source} on ${t.thread_name || `tid ${t.tid}`}: ${dur}`);
      const a = report.analysis[i];
      for (const f of a.findings.slice(0, 3)) {
        lines.push(`  ${f.relation}: ${f.label} [${f.thread_name || f.tid}] ${Math.round(f.overlap_ns / 1e6)} ms`);
      }
      lines.push(`  unattributed: ${Math.round(a.unattributed_ns / 1e6)} ms`);
    });
    return { path, text: lines.join('\n') };
  } catch (e) {
    return { path, text: `unreadable: ${String(e)}` };
  }
}

export default function App() {
  const [incidents, setIncidents] = useState<Summary[]>([]);
  const [status, setStatus] = useState('idle');

  useEffect(() => {
    const refresh = () => setIncidents(TI.listIncidents().slice(0, 6).map(summarize));
    refresh();
    return TI.onIncidentSaved((e) => {
      setStatus(`saved incident ${e.id}`);
      refresh();
    });
  }, []);

  return (
    <SafeAreaView style={styles.root}>
      <ScrollView contentContainerStyle={styles.content}>
        <Text style={styles.h1}>Thread Inspector</Text>
        <Text style={styles.small} testID="status">
          {status}
        </Text>
        {scenarios.map((s) => (
          <Pressable
            key={s.id}
            testID={s.id}
            accessibilityLabel={s.id}
            style={styles.button}
            onPress={async () => {
              setStatus(`running ${s.id}`);
              await s.run();
              setStatus(`ran ${s.id}`);
            }}
          >
            <Text style={styles.buttonText}>{s.title}</Text>
          </Pressable>
        ))}
        <Text style={styles.h2}>Incidents</Text>
        {incidents.map((i) => (
          <View key={i.path} style={styles.card}>
            <Text style={styles.mono}>{i.text}</Text>
          </View>
        ))}
        <Text style={styles.h2}>Capabilities</Text>
        <Text style={styles.mono}>
          {Object.entries(capabilities)
            .map(([k, v]: [string, any]) => `${k}: ${v.status ?? v}${v.reason ? ` (${v.reason})` : ''}`)
            .join('\n')}
        </Text>
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#fff' },
  content: { padding: 16, paddingTop: 48 },
  h1: { fontSize: 22, fontWeight: '700', color: '#111' },
  h2: { fontSize: 16, fontWeight: '600', marginTop: 20, marginBottom: 8, color: '#111' },
  small: { fontSize: 12, color: '#555', marginBottom: 12 },
  button: { backgroundColor: '#1f4fd1', padding: 14, borderRadius: 8, marginBottom: 10 },
  buttonText: { color: '#fff', fontWeight: '600' },
  card: { backgroundColor: '#f2f3f5', padding: 10, borderRadius: 6, marginBottom: 8 },
  mono: { fontFamily: 'monospace', fontSize: 11, color: '#222' },
});
