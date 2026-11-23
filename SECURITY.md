# Security reporting

## Report confidentially

For a suspected vulnerability, use GitHub's
[private vulnerability report form](https://github.com/baabi-boo/Game-Engine/security/advisories/new)
**only if private vulnerability reporting is enabled for this repository**. This
document does not assert that the feature is enabled.

If that form is unavailable, a confidential reporting channel is pending designation
by the repository owner, `@baabi-boo`. Do not publish vulnerability details, exploit
code, sensitive files, or malicious reproducer inputs in public issues, discussions,
or pull requests. A public request for a confidential channel may state only that
such a channel is needed.

Once a private channel is available, include the affected revision, platform,
relevant component, impact, minimal reproduction, and any suggested mitigation.
Remove credentials and unrelated personal or proprietary data from the report.

## Scope and expectations

Security concerns include untrusted-input memory corruption, unintended file access,
or validation failures that cross a documented trust boundary. Ordinary correctness
defects and feature proposals belong in the issue forms unless their details would
expose a vulnerability.

This repository is a pre-alpha headless foundation. There are no announced supported
release branches, security response deadlines, or vulnerability bounty commitments.
The owner will determine affected revisions and any coordinated fix or disclosure
once a report can be received confidentially.

Treat authored worlds, cooked packages, and saved state as untrusted inputs.
Length limits, checked arithmetic, and validation before publication are required
engineering practices. CRC32 detects accidental corruption; it is not an authenticity
or integrity guarantee against an attacker. Do not use the prototype as a security
boundary or assume its test coverage establishes production security.
