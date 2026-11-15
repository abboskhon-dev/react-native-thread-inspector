# Contributing

Issues and pull requests are welcome. Please follow the [code of conduct](./CODE_OF_CONDUCT.md).

## Setup

Requires Node (see [`.nvmrc`](./.nvmrc)), Yarn 4 and CMake. Native work also needs Xcode and/or
Android Studio.

```sh
yarn          # install
yarn test     # CLI tests + C++ core tests
yarn lint     # ESLint + Prettier (add --fix to fix)
yarn typecheck
```

Run the example app (uses the local library; native changes need a rebuild):

```sh
yarn example start
yarn example ios       # or: yarn example android
```

## Where things live

- `cpp/core/`: recorder, watchdog, incident capture, analysis, export. Platform-independent;
  tests in `cpp/tests/main.cpp`.
- `cpp/rn/`: Fabric and TurboModule adapters (React Native headers).
- `ios/`, `android/`: platform modules, probe posting, Hermes sampling.
- `src/`: TypeScript API. `cli/`: `rnti merge` and its tests.
- `benchmarks/`: overhead harness and recorded results.

## Pull requests

- Keep each PR to one change, and add a test when you fix a bug in `cpp/core` or `cli`.
- Thread-safety changes: also run the C++ tests under sanitizers:
  `cmake -S cpp -B cpp/build-tsan -DRNTI_TSAN=ON && cmake --build cpp/build-tsan && ctest --test-dir cpp/build-tsan`
  (`-DRNTI_ASAN=ON` for AddressSanitizer).
- If a change affects what an incident claims (attribution, timings), say how you verified it:
  device, build type, numbers.
- For API changes, open an issue first.
