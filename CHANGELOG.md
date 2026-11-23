# Changelog

User-visible changes, newest first. `## Unreleased` at the top is the section in progress; a
release retitles it `## <version> — <date>`.

## Unreleased

### Changed

- **The watchdog probes less when the app is idle** (#52)
  - A thread that has used no CPU since its last answer is probed every 500 ms instead of every interval; a stall from idle is still caught within one interval.
  - Probe answers no longer wake the watchdog thread.
  - Idle overhead on a Galaxy S21 drops from about 17 to 6 ms of CPU per second.
  - With `jsStacks: true` the sampler keeps the JS thread busy, so JS probes are not skipped.

### Internal

- The example's scenario deep link takes `?delay=<ms>` (#52).

## 0.3.0 — 2026-11-22

### Added

- **`jsStacks: true` saves JS stacks with every incident** (#41)
  - The Hermes sampler stays on in the foreground and is restarted every 30 s; the samples around a stall are saved in the incident's `js-stacks/`.
  - `rnti merge <incident-dir>` works without a session directory.
  - `start()` reports it as the `hermesSamples` capability.
  - Costs about 6% of one core on an idle Galaxy S21. Paused while a `sampling` or `rnti record` session runs.

### Fixed

- With `jsStacks`, `incident.json` no longer says no stack samples were captured; it points to `rnti merge` (#42).
- **The iOS build no longer fails when Reanimated is installed** (#45)
  - `cpp/core/Platform.h` is now `RntiPlatform.h`; on iOS `"Platform.h"` resolved to Reanimated's `platform.h`.

### Internal

- The Android benchmark has a `stacks` mode, and `analyze.py` takes the baseline mode as a second argument (#41).

## 0.2.0 — 2026-11-20

### Added

- **Native spans for iOS app code** (#33)
  - `RNThreadInspector` (`measure`, `begin`/`end`, `newFlow`, `nameCurrentThread`) from Swift or Objective-C, matching the Android API.
  - The README documents native spans on both platforms.
- **`rnti pull ios --device <udid>` copies from a physical iPhone** (#32)
  - Uses `xcrun devicectl`; the app must be a development build. A simulator UDID still reads the simulator's container.
- **`rnti record start|stop` samples a running app from your computer** (#28)
  - Opt in with `start({ remoteSampling: true })`; Android listens to adb shell only, iOS to a Darwin notification on the simulator.
  - `start()` reports it as the `remoteSampling` capability, `unavailable` on a physical iPhone, which does not listen (#40).
  - `stop()` stops listening (#40).
  - `--device` with an iPhone UDID fails with a clear "simulators only" error (#40).
- **`rnti top` shows context switches per thread on Android** (#27)
  - Voluntary and involuntary switches per second, next to CPU time; a thread woken often for small jobs stands out.
  - Recorded in `session.json` as `voluntary_switches` and `involuntary_switches`; iOS has no per-thread count and leaves them out.

### Fixed

- **Ending a span begun before `start()` no longer records a fake span** (#36)
  - It showed up as an `(unknown)` span from the start of the window, and could be blamed for a stall. Affects JS, Android and iOS spans.
- **Calling `start()` twice no longer leaves a second long task observer running** (#22)
  - With `longTasks: false`, `jsLongTasks` always reads `disabled`, even after a failed start.

### Internal

- Claude Code skills in `.claude/skills/` for the flow, checks, thread model, TurboModules and benchmarks (#34).
- Jest tests for the JS API against a mocked native module; `yarn test` runs them (#22).
- Releases are dated, and `release.yml` takes the notes from the version's CHANGELOG section (#25).

## 0.1.0 — 2026-11-18

First release.

### Added

- **Flight recorder for JS and main-thread stalls** (#16)
  - JS and main thread watchdogs, explicit spans and navigation tracking.
  - Each incident is saved as a report (`rnti.incident/1`) with a Perfetto trace.
  - `readIncident()` returns a typed `IncidentReport`.
- **Optional TurboModule and Fabric adapters, and bounded Hermes sampling sessions** (#16)
  - `maxSessions` option; `sampling.stop()` returns `removedSessions`.
- **`rnti` CLI** (#16)
  - `rnti pull` copies incidents and sessions from an Android device or a booted iOS simulator.
  - `rnti top` summarizes one sampling session; `rnti diff` compares two.
  - `rnti merge` adds sampled JS stacks to an incident.
  - `--sourcemap` names release frames from the composed Hermes source map.
- **CPU time per thread in sampling sessions**, listed by `rnti top` (#19)
- **The Android native library is aligned for 16 KB pages** (#20)

### Internal

- The Android benchmark takes each snapshot in one adb call and reports CPU per thread (#17).
- CI builds the minified release and checks alignment (#20).
