---
name: benchmarks
description: >
  Measuring react-native-thread-inspector's overhead on an Android device with
  benchmarks/android_bench.py and analyze.py, and recording results. Trigger phrases: "benchmark",
  "overhead", "measure cost", "CPU cost", "jank", "android_bench", "analyze.py".
---

# Benchmarks

`benchmarks/android_bench.py` runs the example app in paired, randomized order and measures it from
outside (adb), so the recorder never measures itself: process and per-thread CPU from `/proc`,
RSS, PSS (`dumpsys meminfo`), frame stats (`dumpsys gfxinfo`), JS frames/s from logcat.

## Run

1. Build and install the example **release** APK (`cd example/android && ./gradlew assembleRelease`,
   then `adb install -r app/build/outputs/apk/release/app-release.apk`).
2. One device attached, or set `ANDROID_SERIAL`. Screen on, unlocked, not charging if possible.
3. Run:
   ```sh
   python3 benchmarks/android_bench.py --out benchmarks/results/<date>-<device>-<what>.csv \
     --modes none,off,core,full --workloads active --reps 8
   python3 benchmarks/analyze.py benchmarks/results/<date>-<device>-<what>.csv > benchmarks/results/<date>-<device>-<what>.txt
   ```
   Defaults: `--warmup 5 --window 15 --rest 3` seconds, `--seed 1`.

Modes (`example/src/bench.tsx`, launched as `rntiexample://bench/<mode>/<workload>`):
`none` no spans; `off` spans but the recorder never started; `record` recorder without probing;
`core` recorder and watchdogs; `full` plus TurboModule and Fabric adapters; `sampling` plus Hermes
sampling at 100 Hz; `stacks` is `full` plus `jsStacks`, so use `--warmup 20 --window 30` to
include one 30 s rotation. Workloads: `idle`, `active`.

## Reading results

`analyze.py` prints each metric as the difference from the baseline (`none`, else `off`) with a
bootstrap 95% interval. An interval that spans zero is no measurable difference; say so instead of
quoting the mean. Report n (reps), device, Android version and refresh rate.

## Keep

Commit the `.csv`, `.threads.csv` and `.txt` under `benchmarks/results/` named
`<YYYY-MM-DD>-<device>-<what>`, and quote the numbers in the PR's Evidence.

`cpp/bench/micro.cpp` (`rnti_micro` target) measures recorder calls in isolation; use it for
hot-path changes, not as a stand-in for the device run.
