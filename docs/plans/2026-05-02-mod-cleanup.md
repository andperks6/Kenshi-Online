# Kenshi Online Mod Cleanup

## Goal

Make `kenshi-online.mod` a minimal multiplayer asset/template package instead
of a replacement set of game starts.

## Current Findings

The current binary mod contains old proof-of-concept scaffolding:

- Custom `Singleplayer` and `Multiplayer` game starts.
- Vanilla game starts marked `REMOVED`.
- `Player 1` and `Player 2` factions.
- `Player 1 squad` and `Player 2 squad`.
- `Player 1` and `Player 2` character templates.
- `startoff- Wanderer squad copy`, currently wired to `Player 1` with quantity
  20 in the unpatched file.
- Test dialogue text `TEST7ER`.

This does not match the stated 16-player architecture. The old design assigns
slot-specific FCS records for Player 1 and Player 2, with comments elsewhere in
the repo saying to add more records for Player 3-16 later. That is useful for a
two-player prototype, but it is the wrong long-term model.

## Target Shape

- Keep vanilla starts visible and unchanged.
- Do not require a special start for hosting or joining.
- Keep at most one explicit `Kenshi Online Test` start if deterministic test
  fixtures are still useful.
- Keep only generic multiplayer templates that the DLL/server can assign at
  runtime.
- Do not model 16 players as 16 baked FCS starts/squads unless reverse
  engineering proves the engine needs fixed template records.

## Recommended FCS Edits

Use Forgotten Construction Set for structural edits. Do not hand-edit record
removal in the binary file.

1. Open `kenshi-online.mod`.
2. Restore vanilla game starts by removing the mod's `REMOVED` overrides for
   vanilla starts.
3. Delete or disable the custom `Singleplayer` start.
4. Delete or disable the custom `Multiplayer` start unless we rename it to
   `Kenshi Online Test`.
5. Remove `startoff- Wanderer squad copy` if no retained test start uses it.
6. Remove the test dialogue line containing `TEST7ER`.
7. Keep `Player 1` / `Player 2` templates only if current spawn code still
   requires them; otherwise replace them with generic remote-player template
   records.
8. If templates are retained, document their FCS IDs in this file and in the
   spawn code that consumes them.

## Safe Tooling

`tools/audit_mod_assets.py` is intentionally read-only. It extracts printable
strings and high-signal references from the binary mod so we can verify what FCS
saved without relying on manual memory of the editor session.

Run:

```powershell
python tools\audit_mod_assets.py
```

`tools/fix_mod_gamestarts.py` is a narrow legacy repair for the accidental
21-character start. It should not be extended into a general FCS editor.

## Animation Dependency

The mod cleanup is independent from first-pass animation sync. The first useful
animation milestone only needs a local game session to validate the live
animation/movement fields. A second client is needed later when applying the
validated fields to a remote actor.
