# Contributing

Start with [the README](README.md), [agent guidance](AGENTS.md), and the
[architecture overview](docs/architecture/overview.md). This is a headless contract
prototype. Describe only capabilities demonstrated by code and recorded checks.

## Build and validate

Required tools are a C++20 compiler, CMake 3.25+, Ninja, and Python 3.11+.
The default build uses standard libraries and performs no network downloads.
See [developer setup](docs/development/setup.md) for host requirements and diagnostics.

```sh
python3 tools/ge.py check
python3 tools/ge.py run
python3 tools/ge.py package
```

Windows may use `py -3` in place of `python3`. The corresponding CMake workflow is:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Configure, build, and test presets share the names `dev`, `release`, and `asan`.
Use `release` for optimized validation and `asan` where the toolchain supports the
documented sanitizer configuration. Keep build, cooked, and package products under
`out/`; do not commit them.

Run focused checks for changed behavior, then the integration gate above before
publishing a change. Record commands, platform, results, and any checks not run.
Use a meaningful regression test for a behavioral defect; documentation-only edits
need appropriate content and link checks rather than unrelated test additions.

## Keep changes reviewable

Open a bug report for a reproducible defect or a technical proposal for a substantial
new subsystem, dependency, format, or platform. Explain the scenario and acceptance
evidence before expanding scope. Routine fixes can put this rationale in the pull
request. Durable architecture choices belong in an [ADR](docs/adr/README.md).

Keep a pull request focused on one problem. Preserve unrelated work, especially in
a shared checkout. When multiple agents share a checkout, only the coordinating
contributor commits or pushes.

Reviews consider six questions:

1. What problem or measured workload requires the change, and what is outside its scope?
2. What ownership, lifetime, dependency, or publication invariant changes?
3. What evidence verifies behavior, failure paths, and the claimed platform support?
4. What compatibility or migration work is needed for IDs, baselines, APIs, saves, or cooked data?
5. Which memory, work, input-size, or storage budgets change, and how does exhaustion fail?
6. What source, asset, dependency, or generated-content provenance must accompany it?

## Engineering expectations

- Use C++20 and target-scoped CMake. `core` depends only on the standard library;
  `content` and `world` may depend on `core`; applications compose them.
- Keep public APIs in each module's `include/ge/<module>/` tree. Keep vendor types
  and application or editor dependencies out of runtime contracts.
- Distinguish persistent IDs, resident handles, content revisions, and request
  generations. Validate the complete token before publishing delayed results.
- Account for pending, resident, and retiring reservations. Cancellation alone
  does not acknowledge retirement or release a live reservation.
- Bound input and work, validate temporary state before publication, and preserve
  the last valid output when validation fails. Explain changed limits and failure modes.
- Version persistent formats deliberately. Document compatibility, baseline changes,
  migration or rejection behavior, and representative validation fixtures.
- Separate modeled counters from measured memory or timing results. A headless test
  does not establish rendering, physical collision, asynchronous I/O, or frame-rate performance.

## Sources, licensing, and security

Contribute original material or material whose provenance and contribution rights
are documented. Record exact upstream sources, revisions, licenses, transitive
notices, and integration evidence for adopted dependencies or assets. Do not add
hidden downloads, proprietary game material, or large generated assets.

The project license is pending owner selection; this setup introduces no additional
license grant. Read [project policy](docs/project-policy.md) before external reuse
or distribution. Report suspected vulnerabilities through [SECURITY.md](SECURITY.md),
without publishing exploit details in an ordinary issue or pull request.
