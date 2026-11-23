# Third-party register

**No third-party runtime library, source bundle, or binary asset is adopted by the
foundation.** The `adopted` array in [dependencies.json](dependencies.json) is
intentionally empty. The default build uses the C++ and Python standard libraries,
declared build tools, and private operating-system calls described in the
[architecture](../docs/architecture/overview.md).

This directory records adoption decisions; it does not download or vendor anything.
System toolchains, operating-system facilities, and CI actions are accounted for
separately in the [dependency policy](../docs/production/dependency-policy.md#system-tools-and-ci).
An empty runtime register does not replace their applicable terms.

## Candidates under evaluation

Every entry below has status `evaluate`. These are possibilities from the
[public research shortlist](../docs/research/open-world-blueprint.md#a-practical-initial-implementation-stack),
not integrated features or a tested compatible distribution. The JSON register
contains each candidate's purpose, fallback decision owner, and acceptance gates.
No revision has been selected and no license approval is recorded for these candidates.

| Candidate and official upstream | Proposed use |
| --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | Window, input, and platform-event adapter |
| [Jolt](https://github.com/jrouwe/JoltPhysics) | Collision and rigid-body simulation adapter |
| [Recast/Detour](https://github.com/recastnavigation/recastnavigation) | Navigation cooking and bounded runtime queries |
| [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) | Skeletal animation cooking and sampling |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Offline mesh preparation and optimization |
| [Dear ImGui](https://github.com/ocornut/imgui) | Developer diagnostics and authoring interface |
| [Tracy](https://github.com/wolfpld/tracy) | Optional development profiling |
| [RenderDoc](https://github.com/baldurk/renderdoc) | External graphics capture and inspection |
| [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) | Transport only if a networked product is separately selected |

Main-license descriptions in the research are research evidence only. Adoption must
review the selected immutable revision, its enabled optional components, bundled
dependencies, samples, and assets; a current branch URL is not an adoption pin.

## Making an adoption decision

Follow the [adoption checklist and register format](../docs/production/dependency-policy.md#adoption-checklist).
Record exact source provenance, license files and transitive notices, the owner and
update policy, and actual integration evidence in the same change that adopts the
component. Keep vendor types behind the accepted module boundary. Include required
notices with any package containing the adopted material.

The [ownership matrix](../docs/production/ownership.md) identifies the current
fallback reviewer. The [project license remains pending owner selection](../docs/project-policy.md#license-decision);
this register grants no project or third-party license.
