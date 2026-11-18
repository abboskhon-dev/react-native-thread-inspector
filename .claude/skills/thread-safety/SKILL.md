---
name: thread-safety
description: >
  The recorder's thread model in react-native-thread-inspector: who writes, who reads, what may
  block, and how to verify a change with TSan/ASan. Use before touching cpp/core (Recorder,
  EventRing, Watchdog, IncidentController, IncidentWriter, Inspector) or code that posts to the JS
  or main thread. Trigger phrases: "race", "lock", "atomic", "thread", "TSan", "watchdog", "ring".
---

# Thread safety

The inspector records from threads it does not own (JS, main, RN native, app workers) and must
never make them wait. The watchdog must keep working while the thread it watches is stuck.

## Who does what

| Piece | Thread | Rule |
| --- | --- | --- |
| `EventRing` | one producer per ring, any readers | Single producer, lock-free; readers never wait. Torn or lapped slots are counted as `skipped`, not retried. |
| `Recorder::record/begin/end/instant/complete` | any caller | `noexcept`, no allocation after the thread's first event. Each thread binds one ring from a pool of `maxThreads` (16); past that, events are dropped and counted in `droppedNoRing`. |
| `Recorder::recordControl` | any | Separate control ring under a mutex, for watchdog, incident and lifecycle events, so floods cannot evict them. |
| `Recorder::intern`, `nameThread` | any, not hot | Mutex. Callers intern once and cache the id (`RNThreadInspector` on both platforms does). |
| `Watchdog` | its own thread | Posts probes to JS/main through a `Poster` and detects unanswered ones. Never waits on the observed thread. |
| `IncidentController` | watchdog thread + callers | Mutex for incident state; `screen_` is atomic. |
| `IncidentWriter` | its own thread | Snapshots are written off the hot path (`incident-<session>-<id>/`). |
| `Inspector` | process-wide | Installed once and **never destroyed**: any thread may hold a pointer from `Inspector::shared()` at any time. Don't add teardown. |

## Platform threads

- Hermes sampling start/stop and the remote-sampling commands run on the **JS thread**
  (`postToJs` on iOS, `runOnJSQueueThread` on Android), like the JS API's calls.
- Android runs `void` TurboModule methods asynchronously on `mqt_v_native`; timing methods return
  a value so they stay synchronous on JS (see the comment in `src/NativeThreadInspector.ts`).
- iOS keeps the JS `CallInvoker` behind `gInvokerMu`; Darwin notifications arrive on the main thread.
- Fields written by one thread and read by another in Kotlin are `@Volatile`.

## Rules for changes

- Nothing on a recording path may lock a mutex that the writer or watchdog can hold for long, or
  allocate per event.
- A new counter shared between threads is an `std::atomic`, `memory_order_relaxed` unless it
  publishes other data.
- Add a test in `cpp/tests/main.cpp` with real threads for the case you changed.

## Verify

```sh
cmake -S cpp -B cpp/build-tsan -DRNTI_TSAN=ON && cmake --build cpp/build-tsan && ctest --test-dir cpp/build-tsan
cmake -S cpp -B cpp/build-asan -DRNTI_ASAN=ON && cmake --build cpp/build-asan && ctest --test-dir cpp/build-asan
```

CI runs both (`sanitizers (TSAN)`, `sanitizers (ASAN)`). A TSan report is a bug, not noise.
