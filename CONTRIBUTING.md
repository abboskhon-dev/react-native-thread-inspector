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
- `src/`: TypeScript API. `cli/`: the `rnti` commands and their tests.
- `benchmarks/`: overhead harness and recorded results.

## Issues

Open an issue before a big pull request, and link it from the PR (`Closes #123`). A change is big
when it does any of these:

- adds a feature or a new adapter;
- changes the public API (`src/`), the `rnti` CLI, or a file schema (`rnti.incident/1` and friends);
- changes what an incident claims: detection, attribution, timings, or unknowns.

Small fixes, tests, docs and refactors don't need an issue. Agreeing on the approach in the issue
first saves a rewrite in review.

## Pull requests

- Name the branch `feat/<slug>` when it adds or changes behaviour, or `chore/<slug>` for tooling,
  CI, docs and housekeeping. If it does both, use `feat/`.
- Fill in the PR template: summary, evidence, merge danger.
- Add an entry to `CHANGELOG.md` under `Unreleased`: a bold headline with the PR number, details as sub-bullets; build and test changes go under `Internal`.
- Keep each PR to one change, and add a test when you fix a bug in `cpp/core` or `cli`.
- Thread-safety changes: also run the C++ tests under sanitizers:
  `cmake -S cpp -B cpp/build-tsan -DRNTI_TSAN=ON && cmake --build cpp/build-tsan && ctest --test-dir cpp/build-tsan`
  (`-DRNTI_ASAN=ON` for AddressSanitizer).
- If a change affects what an incident claims (attribution, timings), say how you verified it:
  device, build type, numbers.

## Versions and releases

The package follows [semantic versioning](https://semver.org). Until 1.0, a minor version may
break the API and a patch never does.

File schemas are versioned separately from the package. Adding a field keeps the number
(`rnti.incident/1`); removing or renaming a field, or changing what one means, bumps it.

To release:

1. Open a PR that sets `version` in `package.json` and renames `Unreleased` in `CHANGELOG.md` to
   `x.y.z — YYYY-MM-DD`, with a fresh empty `Unreleased` above it.
2. After it merges, tag that commit `vx.y.z` and push the tag.

The tag starts the release workflow: it checks the tag against `package.json`, runs lint, types
and tests, publishes to npm, and creates the GitHub release from the changelog section. A version
with a suffix (`0.2.0-beta.1`) goes to the `next` dist-tag and is marked as a prerelease.
