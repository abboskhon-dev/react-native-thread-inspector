# Changelog

User-visible changes, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

First release.

### Added

- Flight recorder with JS and main thread watchdogs, explicit spans, navigation tracking, and
  incident reports (`rnti.incident/1`) with a Perfetto trace.
- Optional TurboModule and Fabric adapters, and bounded Hermes sampling sessions.
- `rnti merge`: add sampled JS stacks to an incident.
- `rnti top` and `rnti diff`: summarize one sampling session, or compare two.
- `rnti pull`: copy incidents and sessions from an Android device or a booted iOS simulator.
- `--sourcemap` for `rnti top`, `diff` and `merge`: name release frames from the composed Hermes
  source map.
- `maxSessions` option; `sampling.stop()` returns `removedSessions`.
- `readIncident()` returns a typed `IncidentReport`.
- Sampling sessions record CPU time per thread, and `rnti top` lists it.
- The Android native library is aligned for 16 KB pages.
