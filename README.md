# react-native-thread-inspector

A local flight recorder for React Native responsiveness. It keeps a short, bounded history of
instrumented work in your app process and, when the JS or main thread stalls, saves an **incident**:
a report saying what was running and how sure it is, plus a trace you can open in [Perfetto](https://ui.perfetto.dev).

Nothing leaves the device. Labels are static names (spans, module/method names, route names), never
user values or route params.

> Status: research prototype. Validated on a Galaxy S21 (Android 15) and iOS 26/27 simulators with
> React Native 0.88 (New Architecture, Hermes). Not yet published or production-proven.

## What a report looks like

A real incident from a Galaxy S21 release build: navigating to a screen with 6,000 views stalled
the JS thread for 217.7 ms. `incident.json`, trimmed:

```jsonc
{
  "triggers": [{ "source": "js_long_task", "thread_name": "RN JS", "duration_ns": 217743653 }],
  "analysis": [{
    "attributed_ns": 15145962,     // 15.1 ms
    "unattributed_ns": 202597691,  // 202.6 ms
    "findings": [
      { "relation": "direct_containment", "label": "fabric.commit_and_layout", "overlap_ns": 15008039 },
      { "relation": "temporal_association", "label": "navigation.navigate", "overlap_ns": 216971500 }
    ],
    "unknowns": [
      "Part of the interval has no instrumented span on the stalled thread.",
      "No stack samples were captured; work inside spans is not attributed."
    ]
  }]
}
```

The navigation span overlapped the whole stall, but it is an async span, so the report calls it a
temporal association and leaves 202.6 ms unattributed instead of blaming navigation. Merging a
Hermes sampling session into the same incident (`rnti merge`) shows where that time went: 22
samples, 36% in `[HostFunction] createNode`, 18% in `[HostFunction] completeRoot`, most of the rest in
React render.

The full incident is in
[`benchmarks/results/2026-11-15-s21-sampled-big-tree`](./benchmarks/results/2026-11-15-s21-sampled-big-tree/incident-84a9f476-1),
and [RESEARCH.md](./RESEARCH.md) explains what the tool can and cannot prove.

## Install

Not on npm yet. To try it, clone the repo and run the example app:

```sh
yarn
yarn example android # or: yarn example ios
```

Requires the New Architecture and Hermes.

## Use

```ts
import * as TI from 'react-native-thread-inspector';

const capabilities = TI.start({
  incidentThresholdMs: { js: 200, main: 200 },
});

TI.measureSync('feed.parse', () => parse(json)); // span around JS work
TI.mark('cache.miss');                           // instant
TI.trackNavigation(navigationRef);              // React Navigation

TI.onIncidentSaved(({ path }) => {
  const { report } = TI.readIncident(path);
  console.log(report.analysis[0].findings[0]);
});
```

`start()` returns what this build and platform can actually observe, each entry `available`,
`partial`, `disabled` or `unavailable`, with a reason.

### Optional adapters

| Option | Records | Cost / caveat |
| --- | --- | --- |
| `turboModules: true` | Sync calls and async execution of every native module | Installs RN's single global TurboModule perf logger, replacing one another SDK set |
| `fabric: true` | Fabric commit+layout spans, mount-reported instants | Read-only hooks; mount *duration* is not observable |
| `TI.sampling.start(hz)` / `stop()` | Hermes JS stack samples for a bounded session | Separate overhead; Android ignores `hz` (~100 Hz) |
| `useObservedCommit(name)` / `<ObservedCommit>` | Commit timestamp of one instrumented subtree | Not all React commits, not render time, not display time |

## What an incident contains

`incident.json` (`rnti.incident/1`):

- **triggers**: JS watchdog, main watchdog, or JS long task, with onset, detection, end, and what was
  measured (watchdogs measure probe queue delay, not CPU time).
- **analysis** per trigger: findings with a relation —
  `direct_containment` (span on the stalled thread), `explicit_flow` (shared propagated ID), or
  `temporal_association` (only overlapped in time) — attributed vs unattributed time, and a list of unknowns.
- **screen** at detection and transitions in the window.
- **coverage**: per-thread ring stats, overwritten and dropped event counts.

`trace.json` is Chrome trace format; open it in Perfetto.

## Merge Hermes samples into an incident

```sh
npx rnti merge <incident-dir> <session-dir>
```

Writes `trace.sampled.json` (adds a reconstructed JS stack track) and `samples.json` (hot path and
top frames per stall). Absolute bundle paths are stripped from frame names.

## Storage and privacy

- Incidents: app container (`Application Support/rnti-incidents` on iOS, app external files on
  Android), capped by `maxIncidents` (default 20). Sampling sessions keep the latest 5.
- `TI.deleteIncident(path)` and `TI.clearData()` remove saved data.
- iOS privacy manifest declares system boot time (35F9.1) and file timestamp (C617.1) API use; no
  data collection or tracking.

## Limits

- Native work without a span is visible only as unattributed time.
- Android native view mounting time is not observable through Fabric hooks.
- React `<Profiler>` timings are unavailable in standard production builds.
- Overhead budgets are being measured; see `benchmarks/`.

## Contributing

See [CONTRIBUTING.md](./CONTRIBUTING.md). Quick start: `yarn && yarn test`.

## License

MIT
