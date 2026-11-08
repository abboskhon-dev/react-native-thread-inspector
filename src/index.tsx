import Native, { type IncidentSavedEvent } from './NativeThreadInspector';

export type { IncidentSavedEvent };

export type StartOptions = {
  historyMs?: number;
  incidentThresholdMs?: { js?: number; main?: number };
  probeIntervalMs?: number;
  tailAfterRecoveryMs?: number;
  maxIncidentMs?: number;
  cooldownMs?: number;
  longTaskThresholdMs?: number;
  maxIncidents?: number;
  bufferEventsPerThread?: number;
  longTasks?: boolean;
  // Installs RN's global TurboModulePerfLogger; replaces one another SDK (e.g. Sentry) installed.
  turboModules?: boolean;
};

export type CapabilityStatus = 'available' | 'partial' | 'disabled' | 'unavailable';
export type Capabilities = Record<string, { status: CapabilityStatus; reason?: string }>;

export type SpanToken = { labelId: number; spanId: number };

export type ClockCalibration = {
  offsetNs: number;
  // Half the narrowest JS bracket around the native clock read.
  uncertaintyNs: number;
};

declare const performance: { now(): number };

const labels = new Map<string, number>();
let calibration: ClockCalibration | null = null;
let longTaskObserver: { disconnect(): void } | null = null;
let longTaskReason = 'not requested';

function labelId(label: string): number {
  let id = labels.get(label);
  if (id === undefined) {
    id = Native.intern(label);
    labels.set(label, id);
  }
  return id;
}

export function calibrateClock(samples = 7): ClockCalibration {
  let best: { width: number; offsetNs: number } | null = null;
  for (let i = 0; i < samples; i++) {
    const j0 = performance.now();
    const n = Native.nowNs();
    const j1 = performance.now();
    const width = j1 - j0;
    if (!best || width < best.width) {
      best = { width, offsetNs: n - ((j0 + j1) / 2) * 1e6 };
    }
  }
  calibration = { offsetNs: best!.offsetNs, uncertaintyNs: (best!.width / 2) * 1e6 };
  return calibration;
}

export function jsTimeToNativeNs(performanceMs: number): number {
  const c = calibration ?? calibrateClock();
  return c.offsetNs + performanceMs * 1e6;
}

function installLongTaskObserver(): void {
  const PO = (globalThis as any).PerformanceObserver;
  if (typeof PO !== 'function') {
    longTaskReason = 'PerformanceObserver missing';
    return;
  }
  const supported: readonly string[] | undefined = PO.supportedEntryTypes;
  if (!supported?.includes('longtask')) {
    longTaskReason = 'longtask entry type not supported';
    return;
  }
  try {
    const observer = new PO((list: any) => {
      for (const entry of list.getEntries()) {
        Native.reportLongTask(jsTimeToNativeNs(entry.startTime), entry.duration * 1e6);
      }
    });
    observer.observe({ entryTypes: ['longtask'] });
    longTaskObserver = observer;
    longTaskReason = '';
  } catch (e) {
    longTaskReason = `observe failed: ${String(e)}`;
  }
}

export function start(options: StartOptions = {}): Capabilities {
  const capabilities: Capabilities = JSON.parse(Native.start(JSON.stringify(options)));
  calibrateClock();
  if (options.longTasks !== false) installLongTaskObserver();
  capabilities.jsLongTasks = longTaskObserver
    ? { status: 'available' }
    : { status: options.longTasks === false ? 'disabled' : 'unavailable', reason: longTaskReason };
  return capabilities;
}

export function stop(): void {
  longTaskObserver?.disconnect();
  longTaskObserver = null;
  Native.stop();
}

export function newFlow(): number {
  return Native.newFlowId();
}

// An elapsed semantic span; across `await` it does not mean continuous execution.
export function begin(label: string, flowId = 0): SpanToken {
  const id = labelId(label);
  return { labelId: id, spanId: Native.beginSpan(id, flowId) };
}

export function end(token: SpanToken): void {
  Native.endSpan(token.labelId, token.spanId);
}

export function measureSync<T>(label: string, fn: () => T): T {
  const token = begin(label);
  try {
    return fn();
  } finally {
    end(token);
  }
}

export function mark(label: string, arg = 0): void {
  Native.instant(labelId(label), arg);
}

let currentScreen: string | undefined;

// Route names only; params can hold user data and are never recorded.
export function screenChanged(routeName: string): void {
  if (!routeName || routeName === currentScreen) return;
  currentScreen = routeName;
  Native.markScreen(labelId(routeName));
}

export type NavigationContainerLike = {
  addListener(event: 'state' | '__unsafe_action__', listener: (e: any) => void): () => void;
  getCurrentRoute(): { name: string } | undefined;
};

// React Navigation: call from NavigationContainer `onReady` with the container ref.
// The transition span runs from action dispatch to the committed state, so it covers the new screen's render.
export function trackNavigation(container: NavigationContainerLike): () => void {
  let pending: SpanToken | null = null;
  const settle = () => {
    if (pending) end(pending);
    pending = null;
  };
  const report = () => {
    settle();
    const name = container.getCurrentRoute()?.name;
    if (name) screenChanged(name);
  };
  report();
  const offState = container.addListener('state', report);
  const offAction = container.addListener('__unsafe_action__', (e) => {
    if (e?.data?.noop) return;
    settle();
    pending = begin(`navigation.${String(e?.data?.action?.type ?? 'action').toLowerCase()}`);
  });
  return () => {
    settle();
    offState();
    offAction();
  };
}

export function onIncidentSaved(listener: (event: IncidentSavedEvent) => void): () => void {
  const sub = Native.onIncidentSaved(listener);
  return () => sub.remove();
}

export function listIncidents(): string[] {
  return Native.listIncidents();
}

export function readIncident(path: string): { report: any; trace: string } {
  return {
    report: JSON.parse(Native.readIncidentFile(path, 'incident.json')),
    trace: Native.readIncidentFile(path, 'trace.json'),
  };
}

export const debug = {
  simulateNativeBlock: (ms: number) => Native.simulateNativeBlock(ms),
  simulateMainThreadBlock: (ms: number) => Native.simulateMainThreadBlock(ms),
  simulateAsyncNativeWork: (ms: number, flowId = 0) => Native.simulateAsyncNativeWork(ms, flowId),
};
