# Production risk register

All risks remain **open**, adapting the
[research experiments](../research/open-world-blueprint.md#risk-register-and-early-experiments)
to the [first playable](first-playable.md). Owner areas retain the
[current fallback owner](ownership.md). Record revision, configuration, failures,
reviewer, results, and limits in [verification](verification.md).

| Risk / owner area | Early experiment | Gate and mitigation |
| --- | --- | --- |
| **R1 — Real streaming deadlock** / World, host | Constrain actual resource capacity below source-plus-destination overlap; pin source collision, delay uploads/retirement, reverse and cancel requests. | Before M2 streaming acceptance, every transition completes or takes a declared safe loading path within a chosen deadline. Never wait indefinitely for impossible overlap. Reduce dependencies/chunks or stage safely; retain full-token rejection and accountable pin release. |
| **R2 — Modeled bytes hide allocation peaks** / World, rendering | Compare reservation traces with allocator, process, and GPU measurements during decode/upload and deferred retirement. | M1 records measured peaks; M2 enforces declared caps including copies, staging, fragmentation, and retiring consumers. Reconcile unexplained differences and adjust admission estimates. Zero modeled reservations never proves physical release. |
| **R3 — Mesh pipeline cannot support independent iteration** / Content, tools | Independently revise the original mesh; clean-cook it twice, inject invalid indices/units and missing dependencies, then relocate the package. | M1 requires reproducible products, actionable diagnostics, preserved previous output, and correct render/collision alignment. Narrow the supported source subset and version incompatible products before adding asset variety. |
| **R4 — Interrupted saves lose valid state** / World, platform | Terminate processes around write, close, and replacement; inject truncation and failed publication; restart with retained old/new bytes. | Before claiming interruption recovery, recover a complete committed generation or report failure without partial restoration. Preserve baseline/ID checks, immutable capture, serialized writers, and previous valid data. Stronger crash/power-loss durability needs a separately specified and tested protocol. |
| **R5 — Unicode/platform paths break delivery** / Platform, build | Cook, launch, save/restore, and relocate under paths containing spaces, accented and supplementary Unicode characters on each claimed native platform. | Accept only observed target runs with intact paths and diagnostics. Keep native path conversion at OS boundaries; repair encoding assumptions before broadening the supported matrix. CI declarations alone do not qualify a platform. |
| **R6 — Presentation retains stale history** / Rendering | Capture abrupt turns, camera resets, resize, pause/resume, and focus recovery; inspect interpolation and any temporal histories. | M1 requires coherent poses and no stale frame/history reuse. Define reset/invalidation events, bound retained buffers, and disable unqualified temporal features. Repeat after any later history-based rendering integration. |
| **R7 — Competing transform writers or lost support** / Control, physics | Trace fixed-step movement into walls and slopes at declared speed; pause during contact and attempt support-region retirement. | M1 requires one resolved-transform writer, measured contact error within declared limits, and retained support collision. Fix phase ownership, collision layers, or movement bounds before raising traversal speed. |
| **R8 — Maintainer/dependency/license bottleneck** / Ownership, build | A second contributor rebuilds a pinned adapter experiment offline and checks provenance, enabled transitive material, notices, and packaging. | Adoption requires the [dependency record](dependency-policy.md), an accountable maintainer, and retained integration evidence. Keep SDL3/Jolt as evaluation candidates until then; narrow integrations if upkeep cannot be covered. Project license selection remains the owner's decision. |

Foundation checks cannot close these integration risks. M1 covers the bounded
playable route; [later gates](../roadmap.md) govern district expansion and production
qualification.
