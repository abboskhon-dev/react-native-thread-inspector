---
name: flow
description: >
  How a change ships in react-native-thread-inspector: issue, branch, PR, changelog, merge.
  Use at the start of any feature, fix or chore, and before committing or opening a PR.
  Trigger phrases: "start on issue", "implement", "fix", "open a PR", "commit", "merge", "release".
---

# Flow

Never commit on `main`. The ruleset blocks direct pushes; every change lands as a squash-merged PR.

## Steps

1. **Issue.** Find or open one. A one-line issue is enough for a small fix. Big changes (new
   feature or adapter, public API, `rnti` CLI, file schema, what an incident claims) agree on the
   approach in the issue first.
2. **Branch.** `feat/<slug>` for behaviour, `chore/<slug>` for tooling, CI, docs. Both? `feat/`.
3. **Change + test.** A bug fix in `cpp/core` or `cli` comes with a test that fails without it.
   Public API changes update `src/__tests__/index.test.ts`.
4. **Docs.** README for anything a user sees (options, CLI flags, limits).
5. **CHANGELOG.md**, under `## Unreleased`:
   - `### Added / Changed / Fixed / Internal`; build and test changes go under `Internal`.
   - A bold headline ending in `(#PR)`, details as sub-bullets. The PR number is the next
     issue/PR number (`gh api repos/{owner}/{repo}/issues -X GET -f state=all -f per_page=1 --jq '.[0].number'` + 1).
6. **Checks.** Run the `post-work-checks` skill.
7. **Commit.** Subject only, at most 50 characters, imperative, no gitmoji, no body.
8. **PR.** Body starts with `Closes #N`, then the template:
   - **Summary** with a short diff sketch.
   - **Evidence**: device, build type, numbers; and a **Not checked** list.
   - **Merge danger**: Door (one-way or two-way) and Blast radius.
9. **Merge.** Only after CI passes and the maintainer says so. Squash with the PR title as the
   subject, so it ends in `(#N)`; delete the branch.

## Claims about incidents

If a change affects attribution, timings or unknowns, the PR says how it was verified on a real
build (device, release or debug, numbers). Simulator-only or emulator-only is stated as such.

## Release

A PR sets `version` in `package.json` and renames `Unreleased` to `x.y.z — YYYY-MM-DD` with a new
empty `Unreleased` above. After merge, tag `vx.y.z` and push the tag; `release.yml` publishes and
extracts the notes from that section. File schemas (`rnti.incident/1`) bump only when a field is
removed, renamed or changes meaning.
