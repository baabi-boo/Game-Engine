# Dependency and provenance policy

This applies the [project policy](../project-policy.md#dependencies-and-source-provenance)
and [offline-build decision](../adr/0002-cpp20-offline-targets.md). Keep the process
proportional: a focused proposal, a complete provenance record, and evidence for the
actual workload are sufficient. A new committee or recurring approval meeting is not
required.

## Current inventory

[third_party/dependencies.json](../../third_party/dependencies.json) is the adoption
register. Its `adopted` array is empty: the foundation adopts no external runtime
library, vendored source bundle, or binary asset. All listed candidates have status
`evaluate`; none has an approved revision, license review, or integration result.
Their proposed uses and workload-specific gates are recorded in the register.

The public [research shortlist](../research/open-world-blueprint.md#a-practical-initial-implementation-stack)
is design input. Its license descriptions and branch links are research references,
not approvals for the material in a future build. Review exact selected revisions
and their complete contents before adoption. The
[project license is still pending owner selection](../project-policy.md#license-decision).

## System tools and CI

These prerequisites are separate from project-adopted libraries and assets. They
are provisioned by the developer or CI host and retain their applicable terms.

| Prerequisite | Current use and source of configuration |
| --- | --- |
| C++20 compiler and its standard library | Native targets; see [root CMake configuration](../../CMakeLists.txt) and [module boundaries](../architecture/overview.md#dependency-direction). |
| CMake 3.25+ | Configure, build orchestration, CTest, installation, and CPack. |
| Ninja | Generator used by the [CMake presets](../../CMakePresets.json); the CI Visual Studio generator is a separate Windows path. |
| Python 3.11+ and its standard library | Developer commands, cooking, repository checks, and packaging verification. No Python package installation is required by the foundation. |
| Host operating system and toolchain SDK | Filesystem and process facilities, including the private save-publication operation in `ge_world`; these are not vendored engine dependencies. |
| Git | Obtaining and reviewing source; no additional runtime dependency is implied. |

The [CI workflow](../../.github/workflows/ci.yml) is the source of truth for action
commit pins, runner images, and declared compiler jobs. Keep action references pinned
to full upstream commits and verify an update against its official upstream before
merging it. [Dependabot](../../.github/dependabot.yml) proposes GitHub Actions updates;
that does not approve them or update runtime libraries. Runner labels and minimum
tool versions are not an immutable whole-toolchain lock. Record actual versions and
results with each verification run; the configured matrix alone is not passing evidence.

## Adoption checklist

1. **Establish the need and boundary.** Name the next workload, the candidate gates,
   the adapter and its owner, supported targets, resource limits, and failure behavior.
   Preserve the [module dependency direction](../architecture/overview.md#dependency-direction):
   `ge_core` remains standard-library-only. Document a durable dependency-strategy or
   platform change in an [ADR](../adr/README.md).
2. **Pin and inspect the material.** Record the official origin and immutable revision;
   use a full commit ID or content digest, with a SHA-256 digest for acquired archives
   and binaries. Inspect the actual enabled components, build-time requirements,
   bundled/transitive code, samples, and assets. Preserve exact license texts and
   required notices, including changes required when the material is modified.
3. **Verify the integration.** Record commands, source revision, host/tool versions,
   configuration, outcomes, and limits. Check the defined workload, resource and
   lifetime behavior, an offline build with provisioned inputs, and installation or
   relocation where affected. Inspect distributed contents and notice inclusion.
   Use the [verification record](verification.md) and relevant
   [quality gates](../quality-gates.md); do not substitute a successful download for
   integration evidence.
4. **Review and record the decision.** The [responsible owner](ownership.md) reviews
   purpose, terms, evidence, and upkeep in the adoption change. Move the entry from
   `candidates` to `adopted` only when the component is actually adopted, with the
   fields below completed. An evaluation or an unmerged proposal stays `evaluate`.

The default configure, build, check, and package paths must not implicitly fetch
dependencies. Any future optional acquisition step must be explicit, documented,
pinned, and integrity-checked. Provision its inputs before invoking the offline
build. Evaluate a candidate without making every contributor install it. A new
external library, optional tool, or imported asset remains subject to provenance
review even if it is supplied outside `third_party/`.

## Register format

The UTF-8 JSON document has `schema_version: 1` and two arrays, `adopted` and
`candidates`. Keep stable, unique `id` values across both arrays. Paths in records
are repository-relative. Change the schema version if field meanings change.

A candidate contains `id`, `name`, `kind`, `status`, `upstream_url`, `purpose`,
`owner`, and a nonempty `decision_gates` array of strings. `status` is exactly
`evaluate`. `upstream_url` identifies the official project. `owner` is the verified
reviewer or current fallback, not an invented team. `kind` describes the intended
use (`runtime_library`, `content_tool`, or `developer_tool` in the current shortlist).
Candidate entries deliberately have no placeholder pins or asserted license approvals.

An adopted entry retains `id`, `name`, `kind`, `upstream_url`, `purpose`, and
`owner`, changes `status` to `adopted`, and requires the following fields. Use
`external_asset` for imported asset material and `build_tool` for a separately
adopted project build tool. System prerequisites above are not duplicated here.

| Required field | Contents |
| --- | --- |
| `exact_revision` | Full immutable source commit ID or `sha256:` content digest. A branch or version tag alone is insufficient. |
| `acquisition` | Object with `origin_url`, `kind` (`source` or `binary`), and `instructions` pointing to versioned acquisition/build instructions. Include `sha256` for each downloaded archive or binary; instructions identify all acquired inputs. |
| `license_files` | Nonempty array of repository-relative paths to the exact applicable license texts archived with the selected material. A license identifier alone is insufficient. |
| `notice_files` | Array of paths to required notices for the main component; an empty array must be justified by the recorded license review. |
| `transitive_notices` | Array of records with `name`, `exact_revision`, `license_files`, and `notice_files` for included transitive components and assets. An empty array means the selected contents were checked and none apply, with that finding in `verification`. |
| `update_policy` | Who watches for updates, the review interval or trigger, compatibility checks, and how to revert an unsuitable update. The recorded owner remains accountable. |
| `integration` | Object naming the `adapter`, supported `targets`, and `distribution` consequences, including which packages contain the material and where required notices are installed. |
| `verification` | Object with `record` (repository-relative evidence document) and `reviewed_by` (verified account). The record covers source integrity, license/transitive review, workload results, exact host/configuration, and unresolved limits. |

Do not invent completed reviews to fill a field. If evidence is missing, retain the
candidate status. On each revision or enabled-feature change, reassess transitive
contents and notices, run the checks affected by that change, and update the record
and package inputs together. A removal must also remove stale build/package inputs
while retaining the decision history in the relevant change or ADR.

## Source assets and large files

The current [authored fixtures](../../assets/README.md) are small, original synthetic
data. Every new source asset needs a nearby provenance record or indexed manifest:
creator/source, original URL or origin, exact revision or digest, applicable rights
and license/notice files, owner, and transformations used to produce derived content.
Include the tool/version and reproducible inputs when generation affects the result.
The record must cover models, textures, audio, fonts, sample data, and imported test
fixtures as applicable. Public availability and a research citation do not establish
permission to copy or redistribute an asset.

Git LFS is **not configured or required** for the current repository. Before adding
large source assets, choose and document storage, immutable identification, access,
retention, checkout/CI acquisition, and reproducible cooking. Compare ordinary Git,
Git LFS, or external object storage against the actual asset size and workflow; record
a strategy change in an ADR. Do not add LFS as an unused prerequisite. Generated
cooks, builds, and packages belong under `out/` and remain outside source commits.
