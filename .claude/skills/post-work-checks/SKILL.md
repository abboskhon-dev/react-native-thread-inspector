---
name: post-work-checks
description: >
  Checks to run after a change and before opening a PR in react-native-thread-inspector, in order,
  and which native builds CI covers. Trigger phrases: "before PR", "run tests", "lint", "typecheck",
  "sanitizers", "build the example", "does it compile".
---

# Post-work checks

Run in this order; stop at the first failure.

```sh
yarn lint          # ESLint + Prettier; `yarn lint --fix` formats
yarn typecheck     # tsc
yarn test:cli      # node --test cli/*.test.mjs
yarn test:js       # Jest, against a mocked native module
yarn test:cpp      # cmake Debug build of cpp/ + ctest
```

`yarn test` runs the three test steps together.

## When to run more

- **Threads, atomics, rings, the watchdog or the writer changed:** sanitizers.
  ```sh
  cmake -S cpp -B cpp/build-tsan -DRNTI_TSAN=ON && cmake --build cpp/build-tsan && ctest --test-dir cpp/build-tsan
  cmake -S cpp -B cpp/build-asan -DRNTI_ASAN=ON && cmake --build cpp/build-asan && ctest --test-dir cpp/build-asan
  ```
- **`ios/` changed:** build the example for the simulator. After a podspec change run
  `pod install` in `example/ios` first (move `example/ios/build-*` aside if it fails reading an
  `Info.plist` there).
  ```sh
  cd example/ios && xcodebuild -workspace ThreadInspectorExample.xcworkspace -scheme ThreadInspectorExample \
    -configuration Release -sdk iphonesimulator -derivedDataPath build-release-sim build
  ```
- **`android/` changed:** `cd example/android && ./gradlew assembleRelease`.
- **Behaviour on a device:** the example app takes deep links `rntiexample://scenario/<id>`
  (ids in `example/src/App.tsx`), or `-rntiScenario <id>` as an iOS launch argument. Then
  `node cli/rnti.mjs pull <android|ios> threadinspector.example` and read `incident.json`.

## What CI runs

`lint`, `test` and `build-library` are required. CI also runs the sanitizers and the example
builds (`build-android`, `build-ios`). It does not run anything on a device: device evidence
comes from you and goes in the PR.

clang-format is not enforced; match the surrounding style.
