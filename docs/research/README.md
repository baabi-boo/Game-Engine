# Research inputs

[Open-world blueprint](open-world-blueprint.md) is the original public-source
research document supplied for this repository setup, dated 23 November 2026.
It is preserved as a design input rather than rewritten as an implementation claim.
Its 63 research assignments and 153-reference catalog belong to that earlier study.

The current implementation is intentionally narrower. Follow the current
[architecture](../architecture/overview.md), [roadmap](../roadmap.md) and
[quality gates](../quality-gates.md) for repository status. The research's proposed
workloads, staffing, timelines and frame-rate targets are not measured results.

Public references may document another engine's approach or only a conference
abstract. Preserve those distinctions. A linked document is not permission to
copy its source code, assets, trademarks or restricted SDKs into this repository.

## Build references verified during setup

- [CMake 3.25 preset documentation](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html):
  schema version 6 and workflow presets, used to set the declared minimum.
- [GitHub Actions secure use reference](https://docs.github.com/en/actions/reference/security/secure-use):
  immutable action references and least-privilege workflow configuration.
- Official `actions/checkout`, `actions/setup-python` and `actions/upload-artifact`
  release tags and their resolved commit objects were inspected before workflow pins
  were selected. Comments in the workflow record the reviewed release names.

Revalidate upstream material when adopting a dependency or making a platform claim.
The default foundation build currently vendors and fetches no external runtime libraries.
