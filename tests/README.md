# QMud Test Conventions

This directory contains automated tests registered through CTest and implemented with Qt Test (`QTest`).

## Build and run

Configure with testing enabled (default):

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DQMUD_ENABLE_TESTING=ON \
  -DQMUD_ENABLE_GUI_TESTS=ON
```

Build all tests:

```bash
cmake --build cmake-build-release -j"$(nproc)"
```

Run all registered tests:

```bash
ctest --test-dir cmake-build-release --output-on-failure
```

Run by label:

```bash
ctest --test-dir cmake-build-release --output-on-failure -L unit
```

## Naming and layout

- Test target and source naming: `tst_<ModuleName>`.
- Keep tests grouped by category folders ( `gui/`, `integration/`, `regression/`, `smoke/`, `unit/`).
- Common helpers belong in `tests/support/`.
- Static fixture input files belong in `tests/data/`.

## Determinism rules

- No external network or service dependencies.
- Use temporary directories for mutable artifacts.
- Set deterministic locale/timezone when assertions depend on formatting.
- Use `QSignalSpy` and explicit waits instead of timing assumptions for async behavior.

## Labels

- `gui`: widget/window behavior tests.
- `integration`: multi-component tests with controlled fakes/fixtures.
- `regression`: behavior lock for previously fixed bugs.
- `smoke`: minimal wiring tests for build/test plumbing sanity.
- `unit`: fast, isolated logic tests.

## Flaky test triage policy

- Mark unstable tests with label `flaky` and open a root-cause issue link in the test failure report.
- Quarantined tests must be fixed or removed from quarantine.
- Every quarantine entry must include owner, first-failed date, and planned fix path.

## Runtime budget

- Baseline local runtime (Release, GUI tests enabled): `ctest --output-on-failure` is currently ~1 second.
- PR/quick suite budget target in CI: under 15 minutes (`--label-exclude slow`).
- Tests breaching the budget should be labeled `slow` and moved out of the default PR path.

## Coverage and suite milestones

- Milestone 1: keep `unit` + `integration` + `regression` suites green on every pull request.
- Milestone 2: PR CI should always run some GUI tests, the most stable, fast, high-signal ones.
- Milestone 3: publish periodic coverage artifacts from manual workflow runs and track trend over time.
