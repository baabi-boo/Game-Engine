# Working in Game Engine

This is an independent, public-source-informed engine foundation. Read `README.md`,
`docs/architecture/overview.md`, and the relevant module contracts before changes.
Only describe a capability as implemented when code and a meaningful check prove it.

## Engineering rules

- C++20, CMake 3.25+, Python 3.11+; the default build must not download dependencies.
- Use target-scoped CMake. `core` depends only on the standard library; `content`
  and `world` may depend on `core`; applications compose them. No reverse imports.
- Public headers live in each module's `include/ge/<module>/`; implementation is
  private in `src/`. Keep vendor types out of core contracts.
- Persistent IDs, runtime handles, content revisions and request generations are
  distinct. Check the full token before publishing delayed work.
- Memory admission counts pending, resident and retiring reservations. Retirement
  acknowledgement, not cancellation alone, releases a live reservation.
- Parse and validate input into temporary state before publishing it. Bound lengths
  and allocations; preserve the last valid output when validation fails.
- Do not add external dependencies or generated assets casually. Record exact
  provenance, revision, licenses, purpose and integration evidence when adopted.
- Do not claim the synchronous policy fixture is a renderer, collision engine,
  asynchronous I/O system, production save protocol, or a performance benchmark.
- Use supplied/public research with attribution; no proprietary source or assets.
- Keep build products under `out/` and out of Git. Preserve unrelated changes.
- Run the focused checks for changed behavior and the documented integration gate
  before publishing. Report failures and platform limitations accurately.
- Only the coordinating contributor commits or pushes when agents share a checkout.

## Commands

The supported entry point is `python3 tools/ge.py`; Windows may use `py -3`.
See `README.md` for bootstrap/build/check/package commands and raw CMake equivalents.
Do not invent passing results for workflows that have not run.
