# Authored fixtures

`fixtures/district.world.json` is original synthetic data created for this repository.
It contains three regions and four scalar-state objects. It includes no third-party
models, textures, sounds, game data or proprietary material.

The region budgets are **64 modeled bytes each**, deliberately tiny to force admission
failure and retirement under a 96-byte test allowance. They are accounting fixtures,
not measurements of the process heap. Positions are signed integer millimetres.

Edit source fixtures here. Put cooked products under `out/`; do not commit them.
The baseline ID represents the persistence compatibility contract. Changing an
object's stable ID is a content migration, not an ordinary visual rebuild.

Large future source assets need a documented storage and provenance strategy before
adoption. Git LFS is not currently required and no LFS objects are checked in.
