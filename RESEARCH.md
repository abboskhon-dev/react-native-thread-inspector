# What a React Native freeze recorder can prove

A case study in building a local "flight recorder" for React Native responsiveness, and in
being honest about where its evidence runs out.

November 2026. React Native 0.88.1 (New Architecture, bridgeless, Hermes). Tested on a Galaxy
S21 (Android 15, release build) and iOS 26/27 simulators.

## The question

When a React Native app freezes in a release build, the usual tools are either gone (DevTools is
a development tool) or expensive to run continuously (system traces, sampling profilers). By the
time someone reproduces the freeze with a profiler attached, the interesting part is over.

So the question was narrow:

> Can a small in-process recorder keep a few seconds of history, notice a JS or main-thread
> stall on its own, and save a report that says what was running, without claiming more than
> it saw?

The last part turned out to be the hard part. Detecting a stall is easy. Saying *why* it happened
is mostly not possible, and the useful work was making the tool say so precisely.

## Related work

This is not a new category. The comparison below is based on each tool's documentation, not on
running them side by side. Sentry's React Native SDK documents hang and long-task detection,
profiling and TurboModule timing, and keeps adding to them; Margelo's release profiler captures Hermes profiles in release builds;
Android's `ProfilingManager` and Perfetto give system-level traces; MetricKit reports hang
diagnostics on iOS; Flashlight, Shopify's performance library and others measure screens and
frames. The claim here is not "nobody does this". It is a specific combination: local only,
history captured *before* the stall, explicit evidence relations, and tests that check the
report doesn't overclaim.

## What was built

- **Recorder** (C++, shared by both platforms): one fixed-size ring per thread, 64-byte events,
  labels as interned static strings so user values can't end up in a trace. Writers never lock
  or wait; readers copy with a sequence check and count what they lose.
- **Watchdog**: every 100 ms it posts a tiny probe to the JS queue and the main queue. A probe
  not answered within 200 ms is a stall. It measures *queue delay*, not CPU time.
- **Incident capture**: on the first stall, snapshot the last 5 s of every ring immediately,
  keep recording until recovery plus 1 s, merge the two snapshots, analyze off the hot threads,
  write `incident.json` and a Perfetto-compatible `trace.json`.
- **Adapters** (opt-in): Fabric commit/mount hooks, TurboModule perf logger, bounded Hermes
  sampling sessions with an offline merge CLI, navigation tracking (route names only).

## How it was tested

- 20 C++ tests for the core (rings, watchdog, lifecycle, attribution, loss), run plain and under
  AddressSanitizer and ThreadSanitizer, 5 repeats.
- 7 tests for the merge CLI.
- Scripted scenarios in the example app on the S21 and the simulators: a 300 ms JS loop, a
  250 ms main-thread block, a synchronous native call, async native work (must *not* stall), app
  backgrounded mid-hang, cold start, and a heavy screen with ~6,000 views.
- Overhead: an external harness over adb (CPU ticks, RSS/PSS, frame stats), randomized order,
  paired comparisons, bootstrap confidence intervals.

## Finding 1: detection is the easy part, but "easy" hid two bugs

Stalls were detected on both threads in every scripted scenario, and async native work correctly
produced no incident. Two cases needed fixes:

- **Stall before the first probe answer.** At cold start there is no answered probe yet, so the
  tool didn't know which thread was stuck and attributed nothing. Fix: register the JS thread up
  front and use it as a hint.
- **Late watchdog.** If the watchdog thread itself woke late (CPU contention, a little core), it
  could find a probe that had *already* been answered after 300 ms, record the latency and move
  on, with no incident. An external review reproduced this from the state machine. Fix: a completed
  probe over the threshold now opens and closes an incident, marked "detected only after
  recovery". A regression test holds the watchdog asleep deterministically.

Lesson: a detector also needs to handle being late itself.

**What the duration means.** The reported stall is the delay of one probe: from posting it to it
running on the stalled thread. That is a lower bound, not the length of the blocking work. In the
S21 main-thread scenario a 299.8 ms block (measured by its own span) was reported as 250.4 ms:
the block began 0.1 ms after a probe was answered, and the next probe (50 ms interval in the
example app) was posted 50 ms into it. The true stall lies between the reported delay and the
time since the previous answer (here 250.4 to 300.7 ms). For the 300 ms JS loop the report said
300.7 ms: the loop itself ran 300.1 ms (it overshoots a millisecond deadline) plus 0.6 ms of other
JS in the same task.

## Finding 2: attribution needs relations, not overlap

Every report classifies each piece of evidence as one of:

- **direct containment**: a synchronous span on the stalled thread overlapping the stall;
- **explicit flow**: a span elsewhere that shares an ID the code actually propagated;
- **temporal association**: anything that merely overlapped in time.

Stall time is split into **attributed**, **uncertain** and **unattributed**, and the three always
add up to the stall.

Two attribution bugs shaped this:

1. **Async spans counted as containment.** Navigation was tracked with begin/end spans. A
   navigation span stayed open across a render and was reported as *containing* a 294 ms JS
   stall. But an async span covers waiting; other work runs inside it. After the fix, async
   spans are only ever temporal associations. The 294 ms figure was an artifact of the bug.
2. **A lost end looked like "still running".** Rings are fixed size. If a thread wrote enough
   events after a span ended, the end could be overwritten between the two snapshots, and the
   span looked open until capture end: a confident, wrong containment. Now the merge counts
   events lost *during* the incident; spans with reconstructed bounds on a lossy ring go to
   **uncertain**, and the report says how many events were lost and when.

### Results with the fixed build (S21, release)

| Scenario | Stall | Attributed | What the report says |
| --- | --- | --- | --- |
| JS busy loop | 301 ms (JS) | 300 ms | `demo.parseLargeJson`, direct containment |
| Main-thread block (299.8 ms) | 250.4 ms probe delay (main) | 249.6 ms | `debug.mainThreadBlock`, direct containment; block began 50 ms before the probe |
| Heavy screen | 235 ms (JS) | 16 ms | only `fabric.commit_and_layout`; navigation is temporal; ~220 ms unattributed |
| Heavy screen | 701 ms (main) | 0 ms | nothing instrumented ran there; fully unattributed |

The first two rows show the tool is right under known conditions. The last two matter more: on
a realistic heavy screen it explains very little, and says that instead of guessing.

### What the unattributed time was (Hermes samples)

The heavy screen was rerun three times with a Hermes sampling session around it (S21, release,
provenance verified). JS stalls were 218, 230 and 226 ms, with 15-16 ms attributed to
`fabric.commit_and_layout` each time. Of the 65 samples (~95 Hz) outside instrumented spans:

- 52% `[HostFunction] createNode`: Fabric creating shadow nodes, called from React's render phase;
- 26% React render JS (`beginWork`, fiber creation, hooks, a few young-gen GCs);
- 17% `[HostFunction] completeRoot`: React's commit handing the tree to Fabric;
- the rest `appendChild` and a Hermes builtin.

The order was the same in every run: render and `createNode` for the first ~170 ms, then
`completeRoot` until the stall ended, with the Fabric span inside that commit phase. So the
unattributed ~210 ms was React rendering 6,000 views, mostly creating their shadow nodes, not
navigation. Limits: 22-24 samples per stall give shares to roughly ±10 points and can miss phases
under ~20 ms; native time inside `createNode` isn't broken down; and samples cover only the JS
thread, so the 626-666 ms main-thread stalls in these runs stay unattributed.

## Finding 3: React Native internals limit what can be seen

- **Fabric.** Commit hooks fire before Yoga layout and the did-commit callback after, so the
  measurable span is "commit + layout". The mount hook reports *that* mounting happened, not how
  long it took. The 701 ms main-thread stall above is very likely native view mounting, but that
  is an inference; nothing measured it.
- **TurboModules.** React Native has one global perf-logger slot. Installing one replaces
  whatever another SDK installed, so the adapter is opt-in. Async dispatch and execution can't be
  linked (no shared ID). Recording labels exposed what looks like a React Native bug on iOS:
  async execution blocks capture a C-string method name that can be freed before the block runs,
  producing garbage labels. Android copies the string. The adapter drops that name on iOS and
  the JSON writer now rejects invalid UTF-8.
- **Android `void` methods** in TurboModules run asynchronously, so a timing call returning
  `void` would record on the wrong thread.
- **Hermes samples** used the same monotonic clock as the recorder in every configuration tested
  (iOS 26/27 simulators, Android 15 on the S21, RN 0.88.1), so no translation was needed there.
  That is an observation, not a guarantee for other platforms or Hermes versions; the clock anchors
  saved with each session are there to check it. iOS samples at ~650 Hz; Android's API has no rate parameter (~100 Hz).
  A known 300 ms span was reconstructed from samples as 299.1 ms (iOS) and 298.3 ms (S21), with
  edges off by up to about one sampling period. An earlier merge rule stretched the last sample
  up to 10 periods and invented time; fixed.
- **React render timings** aren't available in standard production builds; the tool reports that
  capability as unavailable instead of approximating it.

## Finding 4: overhead

Recording itself is cheap; the always-on watchdog is what costs.

- Per event (microbenchmark): ~7 ns on an Apple M4 Pro; 32–48 ns on the S21's prime core,
  ~211 ns on a little core. A disabled recorder costs 5–8 ns per call on the S21.
- An early version took a span ID from one shared atomic counter per event, which sat on the
  same cache line as the "enabled" flag every event reads. With 4 threads that cost ~195 ns per
  event; per-thread ID blocks brought it to ~7.6 ns.
- Whole app, S21, idle. All modes use the same APK; the baseline `none` never starts the
  recorder (it is not an SDK-free build). Inspector calls made but recorder off: +0.4 ms/s CPU
  [−0.4, +1.5], nothing measurable. Recorder on: +20–26 ms/s across runs, about 2–2.6% of one core.
  RSS +1.0–1.5 MB. Hermes sampling: +110 ms/s while active.
- Where the idle cost comes from: a `record` mode with the same recorder config but probes only
  once an hour cost +0.7 ms/s [−1.4, +2.4] vs `none`; the same run's `core` (probing every 100 ms)
  cost +26.5 ms/s [+14.9, +32.3] (4 paired reps). So the idle cost is the watchdog probing, not
  recording. Memory (~1 MB) is the rings and is the same either way.
- Whole app, busy (locked 120 Hz): unresolved. The baseline now does identical work (same loop,
  same native calls per frame; checked: 1198 calls/s in every mode). But the phone switched
  between ~1170 ms/s at 120 fps and ~1450–1620 ms/s at 97–106 fps as it warmed (31 to 34 °C),
  in every mode including `none`. That swing is 10x any plausible SDK difference. Runs that
  landed in the same state differed by 4–19 ms/s (≤1.3%), but four reps can't establish that.
  Resolving it needs cool-downs, logged CPU frequency/thermal state and more reps.

The "under 1% overhead" target from the original plan is not met at idle, and the cause is now
known: probing. Adaptive probing (back off when nothing happens) is the obvious next step.

## What the tool cannot tell you

- Why uninstrumented code was slow. Unattributed time is a real output, not a failure.
- Native mounting duration, React render duration, or system scheduling (no system trace).
- Causality across threads unless the code passes an ID.
- Anything after the process dies: the history lives in memory until an incident is written.

## Threats to validity

- One Android device, one RN version, simulators rather than iPhones for iOS.
- Scripted stalls are synthetic; the heavy screen is the only realistic workload.
- The Hermes reconstruction check used sessions recorded before provenance checks existed, so it
  was merged with checks disabled.
- Perfetto flagged overlapping trigger slices on one track in the S21 trace; fixed by giving each
  trigger source its own track, and the re-imported trace shows no import errors.
- The overhead baseline is a no-span mode of the same APK, not an app without the library.
- Overhead runs are small (4–8 paired reps) on one phone; the busy-app result is inconclusive
  because of device performance-state changes.
- Watchdog durations are probe delays (lower bounds); stall start is only bracketed.

## Open questions

- Adaptive probing: how low can idle cost go without missing short stalls?
- Can Android's `ProfilingManager` be triggered from an incident to get real system scheduling
  data for the same window?
- Does the iOS method-name issue reproduce on current React Native `main`, and should it be
  reported upstream?
- Would teams actually use local incident files, or does this only make sense with a backend?

## Reproducing

```sh
yarn && yarn test                       # core + CLI tests
yarn example android                     # run the scenarios from the example app
npx rnti merge <incident-dir> <session-dir>
```

Benchmark harness and raw results: `benchmarks/`.
