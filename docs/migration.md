# Migrating Blitz3D Games

FutureBlitz is **not** source-compatible with Blitz3D, and fbengine copies none of Blitz3D's
quirks (the user, 2026-10-31). What moves over from a Blitz game is its assets, converted once to
modern formats, and its behavior, rewritten.

## Assets

fbengine_tools converts a Blitz game's formats when the game is made, and the game stores them
converted: meshes and animations (`.b3d`, `.x`, `.3ds`) to glTF, accounting for Blitz's
left-handed axes and its rotation order, and old image and sound formats to modern ones (fbengine's
plan, E8 step 4). A game's own formats, like SCP: Containment Breach's `.rmesh` rooms, are
converted by that game's tools.

## Code

Whether a translator for `.bb` code is built at all is open (fbengine's docs/questions.md):
recommended is to drop it from phase 2. SCP: Containment Breach is remade from scratch, from its
source's behavior; fbengine's docs/research/ has every Blitz3D command's exact behavior, and
SCP:CB's systems and numbers, for that.

The earlier plan, kept for if it's built later: a deterministic pass for mechanical changes
(syntax, declarations, `Type` to `Class`, engine calls), then an AI-assisted pass for idioms
that need judgment (globals and `Type` lists to classes and collections, `Goto` removal, error
handling), reviewed by a person.

## Final proof

SCP: Containment Breach, remade 1-to-1 at the end of phase 2 (fbengine's E14), feeling nearly
identical, as the capstone validation of the whole stack.
