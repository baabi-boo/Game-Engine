# Project policy

## Status and authority

Game Engine is an independent, public-source-informed headless foundation. Its
[architecture](architecture/overview.md) and [quality gates](quality-gates.md)
define the implemented scope and the evidence needed to expand it. Research targets,
candidate libraries, and planned platforms are not delivered capabilities.

The verified repository owner, `@baabi-boo`, is the fallback code owner. Subsystem
review responsibilities may be assigned as contributors join; no team accounts or
delegated maintainers are implied by this setup.

The repository is public and `main` is unprotected at this setup. `CODEOWNERS` routes
review requests where GitHub applies it; it does not enforce approvals. Workflow
files do not by themselves enable required checks or branch protection. Record any
future repository-setting changes separately from changes to these files.

## License decision

**Project license: pending owner selection; no additional license grant is introduced
by this setup.** No placeholder `LICENSE` or assumed permissive license is supplied.
Ask the repository owner to establish applicable terms before external reuse or
distribution. A contribution or dependency proposal must not imply a license decision
on the owner's behalf.

## Review and expansion

Use a focused pull request to state the problem, changes, validation, compatibility,
resource budgets, and provenance. Use an ADR for durable changes to module boundaries,
persistent identity, file formats, platform commitments, or dependency strategy.
Routine fixes do not need a new governance process.

Subsystem expansion needs a concrete workload, an ownership and lifetime contract,
bounded resource admission and failure behavior, and measurable acceptance evidence.
Separate modeled budgets from actual memory or timing measurements. Document the
host, configuration, input, and limitations of performance claims.

Changes to persistent IDs, baseline identity, saves, or cooked schemas must explain
which versions remain compatible and how incompatible data is migrated or rejected.
Validate into temporary state before publication and preserve valid prior state on
failure. Do not silently reinterpret existing IDs or bytes.

## Dependencies and source provenance

The default build must operate without network access and must not fetch dependencies
implicitly. An optional download or external integration needs an explicit documented
step, pinned input, and a reason the next workload requires it. A candidate named in
research is not an adopted dependency.

For each adopted dependency or external asset, record:

- Component, purpose, upstream URL, exact revision or digest, and responsible maintainer.
- License text location, applicable transitive licenses and notices, and adoption status.
- Source or binary origin, reproducible acquisition/build steps, and update policy.
- Integration evidence, supported targets, and resource or distribution consequences.

Keep required third-party license and notice files with adopted material and include
them in packages that distribute it. The initial foundation has no vendored third-party
runtime source or binary assets. Build tools and standard libraries retain their own
terms; listing them does not grant or replace those terms.

Use original work and public research with attribution. Do not include proprietary
game code, extracted assets, leaked information, private account links, or console
SDK material. Keep authored fixtures small, documented, and reproducible. Generated
build/cook/package products belong under `out/`, outside source commits; large source
assets need an explicit storage and provenance plan before adoption.

## Security and release claims

Follow [security reporting](../SECURITY.md) for suspected vulnerabilities. Private
vulnerability reporting and a confidential contact are not asserted to be active.
No supported release or response-time promise is introduced by this setup.

Publish validation results accurately, including skipped checks and host limitations.
Packaging a headless fixture does not establish production readiness, console support,
graphical capability, asynchronous streaming, power-loss durability, or a frame-rate target.
