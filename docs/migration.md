# Migrating Blitz3D Games

FutureBlitz is **not** source-compatible with Blitz3D, and fbengine is its own engine, not a
modern copy of Blitz3D's (the user, 2026-10-31). Neither the language, its tools, nor the engine
translates Blitz code or converts Blitz assets.

A Blitz game remade on FutureBlitz, as SCP: Containment Breach Ultimate Edition v5 will be after
phase 2 (indistinguishable to play, with a clean codebase of its own), brings its own converters
for its assets, written when that game is, as a separate project tied to neither the language nor
the engine. fbengine's docs/research/ has every Blitz3D command's exact
behavior, and SCP:CB's systems and numbers, for that work.
