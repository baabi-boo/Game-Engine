# Ownership and review responsibilities

The verified repository owner, **`@baabi-boo`**, is the current fallback owner for
every area below. No delegated maintainer or team account is asserted. The matrix
makes review responsibilities explicit while the project is small; it does not
imply additional staffing or implemented roadmap subsystems.

[CODEOWNERS](../../.github/CODEOWNERS) routes review requests to that account where
GitHub applies it. Review routing is not approval enforcement or branch protection.
Authority and license decisions remain as stated in the
[project policy](../project-policy.md).

## Responsibility matrix

| Area | Current owner | Review responsibility |
| --- | --- | --- |
| `engine/core` | `@baabi-boo` | Stable identity, generation-safe handles, bounded time accounting, and the standard-library-only public contract. |
| `engine/content`, `schemas`, content tools | `@baabi-boo` | Bounded parsing, deterministic cooking, format compatibility, validation before publication, and source-to-product provenance. |
| `engine/world` | `@baabi-boo` | Readiness and reservation lifetime, complete request-token checks, persistent baseline/state compatibility, and failure-safe publication. |
| `apps/headless` and integration fixtures | `@baabi-boo` | Application composition, representative workload evidence, and accurate limits on capability and performance claims. |
| Build, developer tools, CI, installation, packaging | `@baabi-boo` | Offline inputs, toolchain declarations, verified action-pin updates, meaningful gates, relocatable outputs, and inclusion of applicable notices. |
| Third-party components and source assets | `@baabi-boo` | Adoption need, immutable provenance, license and transitive-notice review, adapter ownership, update policy, and storage decisions. |
| Tests, architecture, ADRs, research, and project documentation | `@baabi-boo` | Contract coverage, consistency with implemented behavior, attribution, compatibility decisions, and evidence-backed roadmap changes. |

These responsibilities apply to changes in an area, including shared changes that
cross several rows. Future rendering, physics, navigation, animation, editor, or
network integrations acquire an explicit owner and contract when they are proposed;
their appearance in research does not create an adopted subsystem.

## Review and decision handoff

For an ordinary change, identify the affected area and explain the resulting
behavior, focused validation, compatibility and resource consequences in the
[pull request](../../.github/pull_request_template.md). The author gathers the
evidence; the owner reviews the affected contracts and decides whether it is adequate.
Use an ADR for durable boundary, format, platform, identity, or dependency-strategy
decisions. Routine corrections need no separate decision document.

For a dependency or asset adoption, include the complete
[dependency record](dependency-policy.md#register-format), exact license and notice
files, source/asset provenance, and integration evidence before asking for an
adoption decision. Public research references do not authorize copying source or
assets. Preserve the offline default build and identify distribution consequences.
Project license selection is a separate decision for the repository owner.

When a contributor takes ongoing responsibility, verify their account and agreement
first. Update this matrix and relevant CODEOWNERS rules in the same reviewed change.
A handoff should state the scope, module contracts, current verification evidence,
known issues, dependency/update duties, and any outstanding decisions. Until that
handoff is accepted, `@baabi-boo` remains the fallback; a name in an issue or a plan
does not establish delegated ownership.

Source-asset responsibility includes preservation of original inputs and rights
records, reproducible transformations, and package notice inclusion. The owner must
decide a storage strategy before large assets arrive. Git LFS is not currently
configured or required; see [source assets and large files](dependency-policy.md#source-assets-and-large-files).

Report suspected vulnerabilities through [SECURITY.md](../../SECURITY.md). This
matrix adds no confidential reporting channel, release-support commitment, or
response-time promise.
