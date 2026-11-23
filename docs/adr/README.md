# Architecture decision records

These records explain the choices that constrain the current headless foundation
and the conditions for changing them. **Accepted** means a design choice is in
force; it does not mean every later capability or acceptance gate is implemented.
Execution evidence belongs in the [verification record](../production/verification.md).

| Record | Decision | Status |
| --- | --- | --- |
| [0001](0001-headless-foundation.md) | Establish headless lifecycle contracts before a playable feasibility core | Accepted |
| [0002](0002-cpp20-offline-targets.md) | Use C++20, an offline default build, and explicit target boundaries | Accepted |
| [0003](0003-persistent-and-runtime-identity.md) | Scope 64-bit authored IDs to a baseline; keep runtime generations separate | Accepted |
| [0004](0004-time-space-and-authority.md) | Separate clocks, declare coordinates, and give authoritative state one writer | Accepted |
| [0005](0005-content-and-state-formats.md) | Use deterministic, versioned fixture packages and immutable logical snapshots | Accepted |
| [0006](0006-one-native-graphics-backend.md) | Prove one native graphics backend before expanding platform scope | Accepted; backend selection pending |
| [0007](0007-agent-coordination.md) | Coordinate agents with explicit task state, generated handoffs, and source-bound evidence | Accepted |

Each record contains context, the choice, alternatives, consequences, and a
specific revisit trigger. When a decision changes, add a replacement record and
mark the previous one superseded; preserve the reason for the earlier choice.
Link the affected contracts, migration work, and checks in the replacement.

The [architecture overview](../architecture/overview.md) describes current code.
The [roadmap](../roadmap.md) sequences future capability, and the
[quality gates](../quality-gates.md) define the evidence needed to accept it.
The [archived blueprint](../research/open-world-blueprint.md) is research input,
including its historical 63 research assignments. Those assignments are separate
from the repository setup's requested 60 implementation and review assignments;
neither count represents a human team size.
