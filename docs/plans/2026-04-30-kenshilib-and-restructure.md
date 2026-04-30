## Kenshi-Online — KenshiLib swap, GOG+Steam offsets, sync rework

**Date:** 2026-04-30
**Author:** captured during planning conversation
**Goal:** Reduce defensive workarounds by replacing hand-copied offsets with
KenshiLib's typed structs, then move sync onto the game tick and clean up
the spawn pipeline. Keep both Steam and GOG support but drive everything
off pattern signatures so version differences stop being a code path.

---

### Context / motivation

The current codebase (~14k LOC) gets multiplayer working but stability is
held together by SEH wrappers, "does this look like a pointer?" heuristics,
and a spawn pipeline that hijacks NPC creation mid-flight. Recent commits
have done real fix work (v1.0.2 stability pass) but the latest commit drops
7k lines of in-flight engine refactor — the codebase is mid-rebuild.

Most of the runtime fragility traces to three structural choices:
1. **Hand-copied / partly-guessed offsets** with no type system around them.
2. **Background-thread reads/writes** into the game heap, with SEH catching
   the inevitable races.
3. **In-place spawn replay** that depends on hijacking another character's
   factory call.

KenshiLib (BFrizzleFoShizzle fork) provides full typed C++ class definitions
for `Character`, `RootObject`, `Faction`, `GameWorld`, etc., with member
offsets and ~hundreds of method RVAs. Using it as a submodule replaces the
bespoke offset table and gives us a single source of truth.

### Critical finding — rotation offset is wrong

`docs/offsets.json` says `rotation: 0x58`. KenshiLib says
`Ogre::Quaternion rot; // 0xB0 Member` (`Include/kenshi/RootObject.h`).
At offset 0x58 the actual field is `hand handle` (24-byte handle struct
containing pointers).

This explains the "looks like a pointer, skip rotation write" heuristic in
`sync_orchestrator.cpp:42-61`. The existing code reads garbage at 0x58 and
suppresses the resulting bad write rather than reading from 0xB0.

**Verification required before changing.** Could be that the codebase is
actually using a different rotation path (writable_position chain) and 0x58
is dead, or it could be live and explains a real chunk of crashes. Confirm
with a memory dump from a running game before flipping the offset.

### Other field discoveries from KenshiLib

| Field | offsets.json (current) | KenshiLib | Notes |
|---|---|---|---|
| `faction` (owner) | 0x10 | 0x10 | Match |
| `name` (displayName) | 0x18 | 0x18 | Match |
| `gameDataPtr` (data) | 0x40 | 0x40 | Match |
| `position` (pos) | 0x48 | 0x48 | Match |
| `rotation` (rot) | 0x58 | **0x B0** | **Discrepancy — see above** |
| `inventory` | 0x2E8 | 0x2E8 | Match |
| `stats` | 0x450 | 0x450 | Match |
| `squad` (platoon) | -1 (unverified) | 0x658 | New |
| `aiPackage` (ai) | -1 (unverified) | 0x650 | New |
| `moveSpeed` | -1 (unverified) | virtual `getMovementSpeed()` RVA 0x5C7C50 | Use accessor |
| `animState` | -1 (unverified) | `animation` ptr at 0x448 | New |
| `isAlive` | -1 (unverified) | `isDead()` RVA 0x620E30 | Use accessor |
| `currentTask` | -1 (unverified) | TBD via `getMovement()` | Investigate |
| `isPlayerControlled` | -1 (unverified) | `isPlayerCharacter()` RVA 0x790470 | Use accessor |
| `sceneNode` | -1 (unverified) | TBD | Investigate via Ogre side |
| `equipment[14]` | -1 (runtime probed) | per-section in `Inventory` | Investigate |

### Plan

Phases ordered by safety. Each phase is committable and independently
revertible. Verification gates between phases.

#### Phase 0 — Plan + KenshiLib in tree (tonight)
- [x] This plan doc
- [ ] Add `BFrizzleFoShizzle/KenshiLib` as submodule in `lib/kenshilib`
- [ ] Update `offsets.json` with `kenshilib_proposed` annotations on every
      mismatch / new field. Do NOT change runtime offsets yet.
- [ ] Commit

**Stops here for tonight.** No runtime behavior change.

#### Phase 1 — Verify rotation + new offsets against running game (next session, blocked on Ghidra-MCP setup)
- [ ] Stand up Ghidra-MCP locally so Claude can query the binary directly.
- [ ] For rotation: dump memory at `char+0x58` and `char+0xB0` for a known
      character whose rotation is observable in-game. Confirm which is the
      live quaternion.
- [ ] For each field marked `kenshilib_proposed`, validate with a similar
      read-and-print check. Promote to `verified: true` when confirmed.
- [ ] Resolve `sceneNode`, `equipment[14]`, `currentTask`, `weatherState`,
      `buildingList` via Ghidra (these are not in KenshiLib).

**Owner:** Claude with Ghidra-MCP, human reviewing.

#### Phase 2 — KenshiLib submodule + typed accessors (~1 week of focused work, can start in parallel)
- [ ] Replace raw `Memory::Read(charPtr + 0x10)` style code with
      `kenshi::Character::owner` typed accessors.
- [ ] Replace `Vec3` / `Quat` reads with `Ogre::Vector3` / `Ogre::Quaternion`
      to match KenshiLib types (or define a thin shim).
- [ ] Drop the magic-constant pointer validators that have a typed
      replacement (e.g. faction validation → "is this in the faction list?").

#### Phase 3 — Drop GOG-RVA fast path, pattern-driven everywhere (~1 week)
- [ ] Audit `KenshiMP.Scanner/src/orchestrator.cpp` and `patterns.cpp` —
      every place that has `if (pattern fails) try hardcoded RVA`, delete
      the RVA branch.
- [ ] Bump pattern token counts where signatures are too short (the
      comments mention extending to 45 tokens for uniqueness — same trick).
- [ ] Test on Steam and GOG kenshi_x64.exe side-by-side; broaden any
      pattern that fails on Steam.

**Note on binaries:** Kenshi has no DRM on either Steam or GOG. Copy
`kenshi_x64.exe` from each install dir for Ghidra. If only one is owned,
buy the other on sale (~$10) — saves weeks of pattern-discovery work.

#### Phase 4 — Tick-driven sync (1–2 weeks)
- [ ] Hook `GameFrameUpdate` (already exists per Phase 0 docs).
- [ ] Inbound packets: queue from network thread, drain inside tick.
- [ ] Outbound positions: collect on tick, send from network thread.
- [ ] `BackgroundReadEntities` — move squad scan onto tick.
- [ ] Interpolation buffer — feed off tick instead of timer.
- [ ] Delete SEH wrappers that exist only to catch races.

**Validation:** Stress test — 4+ players, zone transitions, combat. Should
crash significantly less than current.

#### Phase 5 — Clean spawn pipeline (2–3 weeks)
- [ ] Replace in-place factory hijack with synthetic request struct +
      direct `RootObjectFactory::create` call (RVA 0x583400, already known).
- [ ] Remove "walk near NPCs to spawn" guidance — spawn should be on demand.
- [ ] Already a partial plan in
      `docs/plans/2026-03-10-clean-puppet-implementation.md` — read first,
      reconcile, replace with clean version.

#### Phase 6 — Type-aware pointer validation (~1 week)
- [ ] Replace `(val > 0x10000 && val < 0x00007FFFFFFFFFFF && (val & 0x3) == 0)`
      style heuristics in `sync_orchestrator.cpp`,
      `pipeline_orchestrator.cpp`, etc.
- [ ] Use KenshiLib types: faction validation = "is this pointer in the
      `Faction*` list of `GameWorld`?"; squad validation = "is this in the
      `ActivePlatoon*` registry?".

### Total effort estimate

| Phase | Estimate |
|---|---|
| 0 — Plan + KenshiLib in tree | tonight (~1h) |
| 1 — Verify offsets | ~1 week with MCP, longer without |
| 2 — Typed accessors | ~1 week |
| 3 — Pattern-driven (drop RVA fast path) | ~1 week |
| 4 — Tick-driven sync | 1–2 weeks |
| 5 — Clean spawn pipeline | 2–3 weeks |
| 6 — Type-aware validation | ~1 week |

**~6–9 weeks** of focused work. Less if Ghidra-MCP is productive; more if
the latest "Update" commit's engine refactor turns out to fight us.

### Risks and watchouts

1. **Rotation offset.** If 0x58 is *intentionally* the rotation offset
   (e.g. a different version of the game) flipping to 0xB0 will break things.
   Verify in-game before changing.
2. **Latest commit (b4924cb) is mid-refactor.** It added 7k lines of
   `engine/`, `sdk/`, `safe_memory.h`, etc. Some of that work overlaps with
   this plan. Reconcile rather than fight — read the new code first.
3. **KenshiLib may target a different Kenshi build.** Verify game version
   it was generated against; if mismatched, treat its offsets as
   `kenshilib_proposed` not gospel.
4. **MCP setup time.** Ghidra-MCP needs Ghidra running with a server plugin.
   First-time setup ~30 min. Without it, RE work falls back to human-driven
   paste-disasm which is much slower.
5. **Don't disable existing SEH wrappers until the underlying race is
   actually fixed.** Otherwise crashes that were caught silently surface
   as hard crashes during the rework.

### What was done in Phase 0 (this session)

- Plan doc (this file)
- `lib/kenshilib` submodule added
- `docs/offsets.json` annotated with `kenshilib_proposed` fields and a
  `kenshilib_source` ref. Runtime offsets unchanged.
- No code changes outside docs and submodule plumbing.
