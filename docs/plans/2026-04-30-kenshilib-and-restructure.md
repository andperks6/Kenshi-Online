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

### Phase 0.5 — Smoke test on 2026-05-01 (blocker found)

Tried to get to `/probe` for the rotation test. Couldn't. Hit a reproducible
crash that blocks any in-game testing. Findings:

**The DLL crashes Kenshi 1.0.68 ~16s into world load.** Signature is
identical across every variation tried tonight:

- `RIP = game+0x59820D` (every time, multiple sessions)
- `AV: READ at 0x0000000000000060` — null `this`, calling a method whose
  first field/vtable lookup is at `+0x60`
- `RAX=0, RCX=0` — null `this` pointer
- Caller at `game+0x60FDB6` (visible at `[RSP+0x58]` in stack frames)
- `Last CharacterCreate: #0, OnGameTick step: -1` — crash happens before
  our `CharacterCreate` hook fires once and before first game tick
- `Filter: inGame=1 inDll=0 inStub=0` — RIP is in Kenshi's own code,
  not in our DLL or our hook stubs
- `R9` varies (3, 4, 6) across sessions — likely a loop iteration count
  inside the crashing function

**What's been ruled out:**

- ❌ NOT the kenshi-online.mod content. Same crash with mod loaded, mod
  not loaded, mod's `Singleplayer` startoff edited (squad 20→1), and
  vanilla scenarios picked instead.
- ❌ NOT the Kenshi 1.0.68 install on F:. Vanilla Kenshi + 7 normal
  community mods (without our DLL or our `kenshi-online.mod`) loads cleanly.
- ❌ NOT a missing terrain plugin. Was missing from `Plugins_x64.cfg`
  earlier in the session but adding `Plugin=Plugin_Terrain_x64` didn't
  fix this crash — different earlier crash (DEP/EXECUTE) was Terrain.

**What's confirmed:**

- ✅ The DLL alone is the trigger. Crash signature appears the moment
  `Plugin=KenshiMP.Core` is enabled, regardless of mod state.

**Strong suspicion** (not verified): the comment at `core.cpp:903-910`
literally warns about this — *"the 130+ rapid-fire CharacterCreate calls
through the MovRaxRsp naked detour corrupt the heap. The hook is only
enabled when connecting to a multiplayer server (via ResumeForNetwork).
Character discovery during/after loading uses CharacterIterator instead."*
But the crash hits during early world init, before any character creation,
so it's likely not the CharacterCreate hook itself but some other hook's
trampoline / a memory probe that corrupts state.

The DLL also intentionally faults ~53 times per session via
`Memory::Read<T>` probes (caught by SEH per `core.cpp:183`). Some of
these run during the loading window.

### Phase 0.6 — Where to pick up next session

Order matters here. Each step is gated on the previous.

1. **Ghidra-MCP: identify what `game+0x59820D` is.** Single highest-leverage
   action. The function's name/contents tells us whether this is a
   character/squad/faction/AI/save-load callee — collapses the bisect
   search space dramatically.
   - Cross-ref: the caller `game+0x60FDB6` is also worth identifying.
   - Distance from `CharacterCreate` (`game+0x581770`): `+0x16A9D` — likely
     a sibling function in the entity/character module.

2. **Hook bisect via env-var gate.** Add a `KMP_DISABLE_HOOKS="hook1,hook2,..."`
   env-var read in `Core::InitHooks` (`core.cpp:882`) so we can flip hooks
   off without rebuilding. Then bisect:
   - Disable everything except `render_hooks` (D3D11 Present — needed for
     chat/HUD) → does Kenshi load cleanly?
   - If yes, re-enable in this order until crash recurs:
     `time_hooks` → `faction_hooks` → `inventory_hooks` →
     `char_tracker_hooks` → `squad_spawn_hooks` → `combat_hooks` →
     `entity_hooks` (CharacterCreate, the suspect) → `squad_hooks` →
     `movement_hooks` → `resource_hooks` → `ai_hooks`.
   - First hook whose enabling reproduces the crash is the culprit.

3. **Once `/probe` works**, do the rotation `/dump` test (Phase 1 of
   original plan: char+0x58 vs char+0xB0).

**Workaround if bisect drags on:** strip the DLL down to only what
`/probe` and `/dump` need — pattern scanner, command registry, Present
hook for chat overlay, no character-side hooks. Build that as a
"probe-only" branch so we can do the rotation verification without
chasing the full crash.

### Test artifacts on disk (session 2026-05-01)

- Crash logs at `<KenshiDir>\KenshiOnline_CRASH.log` — every entry tonight
  is the same crash signature
- Per-PID logs `<KenshiDir>\KenshiOnline_<pid>.log` show DLL initialized
  cleanly, `PollForGameLoad` polls 5-10 times before crash
- User's Kenshi install is at `F:\SteamLibrary\steamapps\common\Kenshi`
  (Steam version, no DRM, no Steamless needed)
- `install.bat` was rewritten this session — single canonical script in
  repo root, mirrored to `dist/install.bat`. Auto-detects Kenshi across
  Steam/GOG locations on C-G drives. Honors `KENSHI_DIR` env override.

### Follow-up: useful mattebin fork changes

Reference branch: `https://github.com/mattebin/Kenshi-Online/tree/coop-stability-2026-04`.

High-signal tracker pieces were selectively ported here:

- `CharAnimUpdate` is allowed to resolve to the intentional unaligned
  mid-function pattern hit.
- `char_tracker_hooks` auto-discovers the Steam `AnimationClassHuman ->
  CharacterHuman` backpointer instead of assuming GOG `+0x2D8`.

Known useful changes to evaluate later, after `tracked > 0` is confirmed:

- `117beed`: shared-save position send should prefer `CharacterHuman +
  character.position` and keep the old animation movement chain only as a
  fallback. Steam appears to break the old anim-chain read.
- Faction-pointer matching in `shared_save_sync`: use tracked
  `CharacterHuman.faction` pointers to disambiguate duplicate placeholder
  names like `Player 1` / `Player 2`.
- `FindByPtr` revalidation for shared-save characters instead of revalidating
  by name, to avoid name-collision regressions.
