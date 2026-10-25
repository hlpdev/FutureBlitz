# Migrating Blitz3D Games

FutureBlitz is **not** source-compatible with Blitz3D. Existing `.bb` code is translated.

## Approach

Two passes:

1. **Deterministic migrator** for mechanical changes: syntax, keywords, operators, block
   endings, declarations, type suffixes → annotations, `Type` → `Class`, engine calls →
   engine API. Fast and always correct.
2. **AI-assisted modernization** for idioms that need judgment: globals + `Type` lists →
   classes with methods and collections, `Goto` removal, error handling. Changes here get
   human review.

This keeps AI mistakes confined to the parts a human should review anyway.

## Timing

The migrator is built **last** (milestone M17): it translates Blitz engine calls into
FutureBlitz engine APIs, so those APIs have to exist first.

## Visual fidelity

Migrated games should look nearly identical. The migrator picks matching render settings
(e.g. `Shading.Vertex`, fog, blend modes); see [engine.md](engine.md#reproducing-the-blitz3d-look).

Old mesh formats (`.b3d`, `.x`, `.3ds`) are converted to glTF by the converter tool,
accounting for Blitz's left-handed coordinates.

## Final proof

Port a real Blitz3D game end to end (e.g. SCP: Containment Breach) as the capstone
validation of the whole stack.
