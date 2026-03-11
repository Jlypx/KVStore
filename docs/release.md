# KVStore v1 Release Baseline

## Purpose

This document defines the current v1 release baseline that can be claimed from the repository as it exists today.
It is limited to checks, binaries, scripts, and saved evidence that are already present in tree.
It does not claim packaging outputs, artifact publication flows, or operator workflows that have not been finalized yet.

## Release baseline in v1

The current releasable baseline is the source tree plus the binaries and evidence that prove the implemented v1 scope works under the repository's existing validation model.

Today, that baseline is anchored by three sources:

1. `scripts/ci/local_check.sh`, which defines the local build baseline as configure, build, and `ctest`
2. `.github/workflows/ci.yml`, which runs the same configure, build, test flow in CI and then runs `bash scripts/ci/local_check.sh`
3. The Task 7 evidence suite under `.sisyphus/evidence/`, which records the current failover, restart, integrity, benchmark, and runtime TLS/profile smoke results

For v1, the binary surface that can be treated as release-relevant is:

- `build/src/kvd`, the current server entry point
- the built test and gate binaries used to reproduce saved evidence, including `kvstore_chaos_gate_test`, `kvstore_integrity_gate_test`, `kvstore_bench_gate_test`, and `kvstore_tls_profile_toggle_test`
- the saved Task 7 JSON and log artifacts under `.sisyphus/evidence/`

This baseline does not include packaged installers, container images, signed artifacts, or deployment bundles.

## Required build/test gates

The minimum release gate in the current repository is the same one used for local and CI validation.

| Gate | Source of truth | Required result |
|---|---|---|
| Configure | `scripts/ci/local_check.sh`, `.github/workflows/ci.yml` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` succeeds |
| Build | `scripts/ci/local_check.sh`, `.github/workflows/ci.yml` | `cmake --build build -j` succeeds |
| Test | `scripts/ci/local_check.sh`, `.github/workflows/ci.yml` | `(cd build && ctest --output-on-failure)` succeeds |
| Failover evidence | `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json` | `pass=true` |
| Restart recovery evidence | `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json` | `pass=true` |
| Integrity evidence | `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json` | `pass=true`, corruption rejected with `CHECKSUM_MISMATCH` |
| Runtime partition-heal and TLS/profile smoke | `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json` | `pass=true`, partition write rejection observed, both `dev` and `secure` smokes pass |
| Benchmark and durability evidence | `.sisyphus/evidence/task7-post-fix/task-7-bench.json` | `pass=true`, `no_acknowledged_write_loss=true` |

The benchmark artifact under `.sisyphus/evidence/task7-post-fix/task-7-bench.json` is the current authoritative benchmark result.
The earlier failed artifact under `.sisyphus/evidence/task7-checks/bench/task-7-bench.json` should be kept for traceability, but it is not the release baseline.

## Evidence package / what to retain

For a v1 release candidate based on the current repository, operators should retain the exact evidence set that proves the checked baseline.

Required retained artifacts:

- `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`
- `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`
- `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`
- `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- `.sisyphus/evidence/task7-post-fix/task-7-bench.json`

Useful supporting artifacts to retain with the release record:

- `.sisyphus/evidence/task7-final/task-7-partition-heal.log`
- `.sisyphus/evidence/task7-final/task-7-tls-toggle.log`
- the CI run that executed `.github/workflows/ci.yml`
- the local or release-candidate run output for `scripts/ci/local_check.sh`

The retained package should let a reviewer answer four questions without guessing:

1. Was the tree configured, built, and tested with the current CI/local baseline?
2. Did failover, restart recovery, and corruption detection pass?
3. Did the live `kvd` binary smoke successfully in both `dev` and `secure` listener profiles?
4. Did the post-fix benchmark keep acknowledged-write-loss protection and remain within the saved acceptance result?

## Runtime smoke expectations

The current runtime smoke expectation is narrow and should be read literally.

- `kvd` must start as `build/src/kvd`
- `kvd` must support `--tls_profile=dev` and `--tls_profile=secure`
- `secure` mode requires readable `--tls_cert` and `--tls_key` inputs
- the external smoke client must be able to connect to a live `kvd` listener and complete `Put` and `Get` in both profiles
- the partition-heal wrapper must show writes rejected during quorum loss and recovery after healing

The checked runtime smoke record for this baseline is `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`.
That artifact shows `pass=true` overall, `partition_write_rejected=true`, and successful external smoke results for both `dev` and `secure` listener profiles.

These smokes validate the current client-facing runtime path.
They do not imply multi-process peer transport, separate inter-node TLS, deployment automation, or a broader production certification program.

## Release checklist and readiness gate

For the current repository, a v1 release claim is ready only when all of the checks below are satisfied against the same tree that produced the retained evidence and release notes.
A passing artifact from an older tree does not count as release readiness for newer code or docs.

| Checklist item | Source of truth | Release-ready result |
|---|---|---|
| Docs baseline | `scripts/docs/check_required_docs.sh` | exits `0`, required Task 8 documents exist, and `docs/release.md` still carries the required core headings |
| Configure | `scripts/ci/local_check.sh`, `.github/workflows/ci.yml` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` succeeds |
| Build | `scripts/ci/local_check.sh`, `.github/workflows/ci.yml` | `cmake --build build -j` succeeds |
| Test | `scripts/ci/local_check.sh`, `.github/workflows/ci.yml` | `(cd build && ctest --output-on-failure)` succeeds |
| Failover evidence | `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json` | `pass=true` within the recorded threshold |
| Restart recovery evidence | `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json` | `pass=true` within the recorded threshold |
| Integrity evidence | `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json` | `pass=true`, corruption is rejected with `CHECKSUM_MISMATCH` |
| Runtime partition-heal and TLS/profile smoke | `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json` | `pass=true`, quorum-loss write rejection is observed, and both `dev` and `secure` smokes pass |
| Benchmark and durability evidence | `.sisyphus/evidence/task7-post-fix/task-7-bench.json` | `pass=true`, `no_acknowledged_write_loss=true`, and the post-fix benchmark remains the authoritative artifact |

Release notes should name the exact evidence files retained for the candidate and should be updated whenever any of the checked inputs change.
If code, commands, or baseline docs change after evidence was captured, the affected gate should be rerun before the tree is claimed as release-ready.
This readiness gate is documented policy today.
The repository now includes a checked-in repo-local helper at `scripts/release/check_release_readiness.sh` to run these readiness checks from the tree itself.
That helper supports local or CI-driven validation, but it is not proof of hosted branch protection or other server-side enforcement.

## Versioning policy for the v1 line

The v1 release line follows Semantic Versioning for versions in the `1.x.y` range.
For this repository, the compatibility boundary is the documented v1 surface, especially the gRPC contract in `docs/wire-protocol.md`, the runtime and operator-facing behavior in `docs/operations.md` and `docs/security.md`, and the release baseline described in this document.

- Increment `MAJOR` for backward-incompatible changes to the documented v1 public contract or release expectations.
- Increment `MINOR` for backward-compatible capability additions or documented expansions of the v1 surface.
- Increment `PATCH` for backward-compatible fixes, internal hardening, documentation corrections, and performance or test changes that do not break the documented contract.
- Pre-release identifiers such as `1.2.0-rc.1` or `1.2.0-beta.1` may be used for release candidates when a stable release is not yet being claimed.
- Once a version identifier is published in release notes or in a git tag, that released content should be treated as immutable. Fixes should ship as a new version, not as a silent rewrite of an old one.

This policy documents how the version number should be chosen.
It does not claim that the repository already has a completed release-tag or publication workflow.

## Changelog policy and template guidance

The repository does not currently carry a committed `CHANGELOG.md` file.
That absence should not block human-readable release notes.
Each release record should still follow a curated changelog structure instead of dumping raw git logs.

Use the current Conventional Commits compatible history as input for note gathering, then write a human-focused summary grouped by change type.
For the current project, the recommended headings are the Keep a Changelog set that best matches the actual release:

- `Added`
- `Changed`
- `Fixed`
- `Deprecated`
- `Removed`
- `Security`

Recommended template for each released version or release-candidate note:

```md
## [1.2.3] - YYYY-MM-DD

### Summary
- one short paragraph or 2 to 3 bullets about why this release exists

### Added
- user-visible additions

### Changed
- behavior or operational changes that stay compatible

### Fixed
- bug fixes and reliability corrections

### Deprecated
- features or paths scheduled for later removal

### Removed
- only include when something actually left the supported v1 surface

### Security
- transport, integrity, or exposure-related notes when applicable

### Verification
- configure/build/test status
- evidence paths for chaos, integrity, runtime smoke, and benchmark gates

### Rollback notes
- previous known-good commit or release identifier, if one exists
- practical rollback limits for the current repository
```

Release notes should use ISO `YYYY-MM-DD` dates, should call out breaking changes plainly, and should mention deprecations before removals when that sequence applies.
If a `CHANGELOG.md` file is added later, it should follow the same structure and keep an `Unreleased` section at the top for work that has not shipped yet.

## Git governance, authorship, and branch strategy

The current repository history already follows a Conventional Commits compatible subject style, with examples such as `feat(engine): ...`, `docs(architecture): ...`, and `chore(repo): ...`.
That style is the project policy for normal commits because it keeps the history readable and makes later changelog and version review simpler.

- The git author identity for project commits should be `jlypx`.
- Commit subjects should remain Conventional Commits compatible, with a lowercase type and an optional scope, for example `feat(api): ...`, `fix(engine): ...`, `docs(release): ...`, or `chore(ci): ...`.
- `BREAKING CHANGE:` footers should be used when a commit introduces an incompatible v1 change.
- Unlabeled changes should not be merged or treated as release-ready when the relevant validation evidence is missing.
- AI or tool authorship must not appear as the git author, must not replace the human author identity, and must not be added through `Co-authored-by:` trailers for project commits.

The branch model is trunk plus short-lived branches.
`main` is the trunk and should stay close to releasable state.
Feature, fix, and docs work should happen on focused short-lived branches, then merge back after the relevant build, test, docs, chaos, integrity, and benchmark checks are current.
This document does not claim that branch protection or server-side commit-policy enforcement is already configured.

## Rollback and release notes expectations

Rollback expectations should match the repository that exists today.
There are no packaged installers, published artifacts, or automated deployment rollback flows in tree, so rollback notes should stay source-based and evidence-based.

Each release note or release-candidate note should include:

1. the release identifier being claimed
2. the previous known-good commit or release identifier, if one exists
3. the exact verification gates that passed, with evidence file paths
4. any compatibility notes, breaking changes, deferred items, or operator-visible caveats
5. the practical rollback path for the current repository, usually reverting to the previous known-good tree, rebuilding with the documented commands, and rerunning the affected gates

If a release candidate is found to be bad after notes are written, the fix should be a new corrective version or a documented revert.
The project should not silently edit older evidence or pretend that package-level rollback automation exists when it does not.

## Known release risks and deferred items

The current release baseline is intentionally practical, but it is not a complete release-governance system.

Known risks or limits in the current baseline:

- the server topology remains one `kvd` process with an embedded static five-node cluster
- validation is local and CI-driven, not a distributed multi-host release program
- runtime smoke is environment-sensitive and was captured through the same WSL-style execution path used in Task 7
- benchmark coverage is an acceptance gate, not a capacity model or long-duration soak result
- deployment assets are still incomplete, `deploy/README.md` says cluster and local deployment manifests will be added in later tasks

This document now defines the release checklist, SemVer policy, changelog structure guidance, git authorship rules, branch strategy, and rollback-note expectations required for Task 9.
The repository also now carries repo-local enforcement helpers at `scripts/git/check_commit_messages.sh` and `scripts/release/check_release_readiness.sh`.
Those scripts help enforce policy from the checked-out tree, but they do not by themselves prove hosted or server-side governance is active.
The following items are still deferred or not yet enforced by repository automation and should not be overstated:

- branch protection or server-side git enforcement rules are not documented as active repository settings
- artifact publication policy is still incomplete
- signing, provenance, or installer/distribution rules are still incomplete
- formal upgrade sequencing and support-window policy are still incomplete

This document only states what can be released on evidence today.
It defines the current governance policy that can be stated truthfully from the repository, while leaving unimplemented publication and enforcement machinery out of scope.

## Validation and references

Primary baseline references:

- `scripts/ci/local_check.sh`
- `scripts/docs/check_required_docs.sh`
- `scripts/git/check_commit_messages.sh`
- `scripts/release/check_release_readiness.sh`
- `.github/workflows/ci.yml`
- `docs/testing.md`
- `docs/operations.md`
- `docs/architecture.md`

Governance policy references:

- `https://www.conventionalcommits.org/en/v1.0.0/`
- `https://semver.org/`
- `https://keepachangelog.com/en/1.1.0/`

Primary Task 7 evidence references:

- `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`
- `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`
- `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`
- `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- `.sisyphus/evidence/task7-post-fix/task-7-bench.json`

Supporting deployment boundary references:

- `deploy/README.md`
- `deploy/kvd_single_process.conf`

If any of the references above change, the release baseline in this document should be updated to match the new repository reality rather than preserving stale release claims.
