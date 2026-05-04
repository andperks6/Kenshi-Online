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

### Current validation status (2026-05-02)

Phase 1 offset validation is complete for the high-risk character fields.
The runtime table has been promoted from proposed KenshiLib values to
verified values, based on live `/validate_offsets` runs against Steam
Kenshi 1.0.68.

Evidence:

- Earlier validation: `98/98` tracked characters plausible for the promoted
  offsets, `0/98` plausible for old rotation `+0x58`.
- Fresh validation in `KenshiOnline_44568.log`:
  - `Samples: 8 / tracked 66`
  - `rot +0x58    0/66 plausible`
  - `rot +0xB0    66/66 plausible`
  - `anim +0x448  66/66 plausible`
  - `ai +0x650    66/66 plausible`
  - `squad +0x658 66/66 plausible`
  - conclusion: rotation is `+0xB0`

Promoted runtime offsets:

| Field | Runtime offset | Status |
|---|---:|---|
| `Character::rotation` | `0xB0` | verified live; old `0x58` rejected |
| `Character::animation` / `animClassOffset` | `0x448` | verified live |
| `Character::ai` / `aiPackage` | `0x650` | verified live |
| `Character::platoon` / local `squad` alias | `0x658` | verified live |

Important distinction: `animClassOffset=0x448` is the animation class
pointer, not the `animState` byte. `animState` remains unknown.

### Character discovery status

`char_tracker_hooks` is now the primary reliable character discovery path.
It observes the animation update path and auto-discovers
`AnimationClassHuman -> CharacterHuman`; latest validation showed
`tracked=66`.

`CharacterIterator` is still present, but should be treated as a diagnostic
and fallback source only. It attempts:

1. `PlayerBase` as a pointer array.
2. `GameWorld + characterList` as a `lektor` dynamic array.

Current logs show `PlayerBase` is not a valid character list on this build,
and the `GameWorld` fallback often exposes only one character while
`char_tracker` sees the full set. Any sync path that needs the local squad
or world character list should prefer `char_tracker` and only fall back to
`CharacterIterator`.

Next code cleanup target:

- Route `Core::SendExistingEntitiesToServer()` and related local-squad sync
  through `char_tracker_hooks::GetTrackedSnapshot()`.
- Keep `CharacterIterator` for `/probe`, `/verify`, offset prober fallback,
  and legacy diagnostics until those callers are migrated or deleted.
- Reduce remaining per-frame iterator log noise; current code logs only when
  iterator source/count changes.

### Cleanup completed (2026-05-02)

- Removed `InitOffsetsFromScanner()`, which was a no-op placeholder that only
  set `discoveredByScanner=false` and logged "Using CE fallback offsets".
- Startup now initializes offsets by touching `game::GetOffsets()`, whose
  defaults are the verified runtime table in `game_types.h`.
- Offset-cache restore no longer overwrites verified defaults for
  `aiPackage`, `animClassOffset`, or `squad`; it only fills those fields if
  they are unknown.

### Animation-state probing (2026-05-03)

Manual `/anim_state zz` capture was run against a controlled test character
in the sequence idle, walk, jog, run, sprint, sneak, idle. The command logged
`AnimationClassHuman` candidate fields plus position in
`KenshiOnline_25364.log`.

Observed values:

| Step | Intended motion | `anim+0x530` | `anim+0x570` |
|---:|---|---:|---:|
| 1 | idle | 3 | 3 |
| 2 | walk | 22 | 22 |
| 3 | jog | 22 | 22 |
| 4 | run | 6 | 6 |
| 5 | sprint | 13 | 13 |
| 6 | sneak | 2 | 2 |
| 7 | return idle | 10 | 10 |

Conclusion: `anim+0x530` and `anim+0x570` are high-signal mirrored fields,
but they are not yet safe to promote as `animState`. The return-to-idle value
did not match the initial idle value, so these are likely action/clip codes or
animation-layer state rather than a stable high-level movement mode. Do not
repeat the same manual state sequence; the next step should be automated
sampling with velocity/stance context so state transitions can be correlated
without hand-entering commands.

Follow-up `/anim_trace zz 12 250` captures added position-derived speed plus
the `AnimationClassHuman +0x328 -> +0x8` controller result buffer from
`FUN_14065f160`. Two 49-sample traces were captured in `KenshiOnline_26356.log`:
one stationary, one moving through walk/run/sprint/sneak. The source buffer did
not expose a usable locomotion enum in this path:

| Field | Observation |
|---|---|
| `anim+0x334` | Constant `2` |
| `anim+0x340` | Constant `11` |
| `anim+0x344..0x350` | Constant zero |
| `anim+0x530/+0x570` | Continues cycling independently of speed |
| `ctrl+0x18` | Constant active `1` |
| `result+0x4..0x7` | Constant flags `0x01010000` |
| `result+0x38` | Constant `11` |
| `result+0x40` | Constant `0` |
| `result+0x50` | Constant `2` |

Position-derived speed cleanly separated the tested movement bands
(idle `0`, walk about `15`, run about `55`, sprint about `70`, sneak about
`23`). First animation-sync milestone should therefore derive and transmit a
compact locomotion state from velocity plus stance/control context, instead of
blocking on a single discovered animation-state offset. Keep the Ghidra notes
for later attack/block/parry work, where combat animation events may need
separate hooks.

### Ghidra follow-up: stance/order actions (2026-05-03)

The lack of a useful locomotion enum in `AnimationClassHuman` does not mean the
game has no stance state. Ghidra points to a different path for toggles such as
sneak/block/passive/taunt/ranged:

- `FUN_1403636c0` registers input actions. Relevant action names and IDs:
  - `toggle_block` -> `0x17`
  - `toggle_hold` -> `0x18`
  - `toggle_passive` -> `0x19`
  - `toggle_jobs` -> `0x1A`
  - `toggle_ranged` -> `0x1B`
  - `toggle_sneak` -> `0x1C`
  - `toggle_taunt` -> `0x1D`
  - `cycle_run_speed` -> `0x1E`
- `FUN_14072b540` constructs the orders panel and wires buttons such as
  `OrdersBlockButton`, `OrdersHoldButton`, `OrdersPassiveButton`,
  `OrdersTauntButton`, `OrdersRangedButton`, and `OrdersSneakButton`.
- `FUN_140721410` is the `OrdersSneakButton` handler. It toggles the MyGUI
  selected state, plays `Stealth` when enabling, and dispatches:
  - `thunk_FUN_1407f3880(DAT_142134690, 3)` when enabling sneak
  - `thunk_FUN_1407f3880(DAT_142134690, 4)` when disabling sneak
- `FUN_1407f3880` is a central selected-character order dispatcher. It iterates
  selected characters and calls virtual method `+0x308` on each character with
  `(orderId, bool)`.
- Other order-panel delegates call the same dispatcher with IDs including
  `0x0C`, `0x0D`, `0x0E`, `0x0F`, and `0x11` for combat/order toggles.

Implication: the first practical sync path should not be a raw animation offset.
It should be split:

1. Locomotion presentation: derive idle/walk/run/sprint bands from position
   delta and transmit them in `CharacterPosition.animStateId/moveSpeed/flags`.
2. Sneak/combat stance: trace or hook the order dispatcher path so toggles can
   set explicit network flags. Sneak should be a real bit, not inferred from
   speed, because sneak speed overlaps slow movement.
3. Remote application: use the same order-dispatch path or the per-character
   virtual `+0x308` target to apply stance changes to remote characters if
   calling it proves stable. Avoid writing arbitrary guessed character flags
   until a real offset is verified.

Implementation note: `order_hooks` now hooks the selected-character order
dispatcher at `game+0x7F3880` and defers logging to `OnGameTick`. Runtime
validation on 2026-05-03 confirmed the hook installs cleanly and logs while the
game is still running. Ghidra button-handler cross-check plus the second-pass
click order mapped the selected-character order IDs:

- `0x0B` = block.
- `0x0C` = hold.
- `0x0D` = passive.
- `0x0F` = jobs.
- `0x11` = ranged.
- `0x0E` = taunt.
- `3` = sneak on.
- `4` = sneak off.
- `0`, `1`, `2`, `0x10` = run-speed cycle states observed while moving.

Code integration:

- `movement_state.h` centralizes first-milestone locomotion classification:
  idle `<0.5`, walk/slow `<3.5`, run `<6.5`, sprint `>=6.5`, with sneak
  overriding to anim state `4`.
- `moveSpeed` packet encoding now maps `0..8` observed polled speed units to
  `0..255`. A Freedom 5 weighted-party test on 2026-05-03 showed slow movement
  around `2.5..3.0` and faster movement around `5.0..6.8`; the earlier
  `/anim_trace` position-derived scale was not the same as
  `Core::PollLocalPositions`.
- `order_hooks` tracks local selected-character toggles and feeds
  `CharacterPosition.flags`: sneak, block, hold, passive, jobs, ranged, taunt.
  Run-speed cycle orders are also preserved as explicit intent flags
  (`CPF_RunSpeed0`, `CPF_RunSpeed1`, `CPF_RunSpeed2`, `CPF_RunSpeed16`) and are
  logged as `runOrder`. These should drive sprint/run presentation once the UI
  order-to-label mapping is validated, because backpack weight can make speed
  alone ambiguous.
  This is intentionally local aggregate state for the first milestone; a later
  per-character vfunc hook is needed if multiple selected characters can diverge.
- Same-party test start (`zz1`..`zz6`) confirmed `char_tracker_hooks` sees local
  party members immediately, while `CharacterIterator` still finds zero squad
  characters and leaves the entity registry empty. `SendExistingEntitiesToServer`
  now has a narrow `char_tracker` fallback for `zz<number>` and `Player <number>`
  names so test/local-template characters can register and exercise movement
  packets. This is not the final faction/ownership solution.

Probe recipe for the next run:

1. Start/load normally, host from F1, wait until `tracked > 0`.
2. Run `/order_trace on`.
3. Select the local test character and toggle Sneak on/off, Block on/off,
   Passive on/off, Hold on/off, Ranged on/off, Taunt on/off, and cycle run
   speed once or twice.
4. Run `/order_trace off` or quit.
5. Inspect the latest `KenshiOnline_*.log` for `order_hooks:` install status and
   `order_trace:` lines. The current log file can usually be read while Kenshi
   is still running; quitting only matters if the file sink has not flushed the
   newest lines yet.

### Trading and inventory reality check (2026-05-03)

Current docs overstate trade/inventory completeness. The live code has protocol
types and partial handlers, but not a real player-to-player trade workflow.

Observed implementation:

- `inventory_hooks.cpp` hooks `ItemPickup`, `ItemDrop`, and `BuyItem`.
- `BuyItem` sends `C2S_TradeRequest` with buyer, seller, item template, quantity,
  and `price = 0`.
- Server `HandleTradeRequest` only validates buyer ownership, quantity, price
  range, and optional seller existence, then broadcasts `S2C_TradeResult`.
- Client `HandleTradeResult` only displays success/denied text.
- `ItemTransfer` exists as a message and server handler, but it only broadcasts
  inventory add/remove updates. There is no consent/offer/accept/cancel state,
  no escrow, no money validation, no duplicate prevention, and no confirmed
  local inventory mutation path for true player-to-player trades.

Implication: player trading is effectively unimplemented. Treat current
inventory/trade code as experimental item-event broadcast scaffolding, not a
safe multiplayer trading feature.

Combat reality check:

- `ApplyDamage` is intentionally not hooked because the mov-rax-rsp wrapper was
  crash-prone under frequent calls.
- Current combat sync relies on death/KO hooks plus limb-health snapshots and
  simplified server combat code for `C2S_AttackIntent`.
- Block/parry/attack animation fidelity is not established. Later work should
  trace task/vtable paths such as `AttackState`/`Task_MeleeAttack`, but it
  should stay separate from the first locomotion/stance milestone.
