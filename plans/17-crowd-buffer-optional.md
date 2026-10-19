# 17 — Crowd buffer (optional)

Only if the Cell run in step 15 shows `view` (phase U2) dominating `wall` even after step 14 chunking. Not in the project's exit criteria.

## Problem it addresses

After step 10 every viewer builds its own update packet from the slots of every visible object. In a crowd of N players that is N² `BuildValuesUpdateFromSlot` calls per beat, each serialising the same changed fields. The per-viewer part that is genuinely different is small: dynamic flags (`UNIT_DYNAMIC_FLAGS` tapped/lootable per viewer, `UNIT_FIELD_AURASTATE` for some auras, `GAMEOBJECT_DYNAMIC` for quest-visible objects) and the viewer's own object.

## Design

- S2 assigns each cell with ≥ `CROWD_THRESHOLD` (32) players a `CrowdBuffer` item for the next beat; one claim per cell.
- The crowd item is a U2 item pushed onto the `heavy` ring so it runs first in phase U2 (step 12), after every U1 task has published. Viewer chunks of the same cell wait on the cell's `ready` flag (a per-cell `atomic<bool>`, set by the crowd item with release; chunks spin-yield on it, which is short because the crowd item started first). The shared block is built from this beat's slots: no added latency.
- It builds one `UpdateData` block per **player** in the cell (creatures are not buffered: their viewers are fewer and their dynamic flags vary more), with the per-viewer indices left as zero and their byte offsets recorded in a side table `std::vector<std::pair<uint16 index, uint32 byteOffset>>`.
- Radius: the buffer covers objects within the shortest visibility distance any viewer in the cell can have (`World::GetMaxVisibleDistanceOnContinents` minus the `VisibilityDistance` aura range extensions, i.e. the minimum a viewer could see). Objects beyond it fall back to the per-viewer path.
- A viewer in the cell copies the shared bytes and patches its per-viewer indices from its own view of the target's slot, then appends per-viewer-path objects (those outside the fixed radius, those whose stealth/invisibility detection differs, and the viewer itself).
- Buffers live in a per-cell `CrowdBuffer` object owned by the map (allocated in S2 when the cell first crosses the threshold, retained at high-water, freed when the map unloads), are reset in S2, and are read by the viewer chunks of the same beat.

## Correctness constraints

- Stealth/invisibility: a target whose `stealth` or `invisibility` arrays are non-zero is excluded from the shared block and always goes the per-viewer path.
- Phasing: a target whose `phaseMask` is not the all-common mask of the cell is excluded.
- Group-only fields (`UNIT_FIELD_HEALTH` for non-group members is public in 3.3.5; `PLAYER_FIELD_*` private fields are per-viewer by `UpdateMask` type): private fields are never in the shared block; the viewer who is the object itself takes the per-viewer path.
- A viewer must never receive the shared block for an object it does not have a `VisibilityLink` to; the viewer filters by its own `_visibleObjects` set before copying.

## Gate

Cell run at full key, before and after: `view` and `view_tail` fall; `wall` falls; no visibility regression in a debug run with three characters where one is stealthed, one is in a different phase, and one taps a mob.

## Checklist

- [x] `CrowdBuffer` item, one claim per cell, first in U2 via the heavy ring, per-cell `ready` flag.
- [x] Players only, ~~fixed radius~~ (per-target blocks, see Status), dynamic indices patched per viewer.
- [x] Stealth / ~~phase~~ (unnecessary, see Status) / private exclusions.
- [ ] Gate numbers in the exit note, or the step dropped with the numbers that justified dropping it — **neither
  is available**: the step 15 Cell run was never made (no build on this machine). The step is built behind
  `MapUpdate.CrowdBuffer` (default off) so the gate can be measured on/off in the same soak without a rebuild.

## Status

Built statically (no compile, no play-test). Off by default; the gate decides whether it stays.

### What landed

| Piece | Where |
|---|---|
| `CrowdBuffer` (per cell: planned guids, one `BuildValuesCachedBuffer` per built player, guid → entry index, `atomic<bool> ready`) | `src/server/game/Maps/CrowdBuffer.h/.cpp` |
| `Unit::BuildValuesCacheEntry` (the field loop of `BuildValuesUpdateFrom`, factored out), `BuildPublicValuesBlock` (`UPDATETYPE_VALUES` + packed guid + `UF_FLAG_PUBLIC` entry from a slot), `SharedBlockApplies`, `AppendSharedValuesBlock` (copy + `PatchValuesUpdate` + `AddUpdateBlock`) | `Unit.h/.cpp` |
| `Map::_crowdBuffers`, `PlanCrowdBuffers` (S2: players per cell via `ComputeCellCoord`, plan ≥ 32, deactivate the rest), `BuildCrowdBuffer`, `FindCrowdBuffer`, `CrowdBuffers()` | `Map.h/.cpp` |
| `WorkItem::Crowd` (`ptr` = map, `chunk` = cell id); `CoreState::crowdItems`; `PlanCrowdItems` in `SealSerial` (round-robin over cores); U2 fills the `heavy` deques from `crowdItems` so every core drains them before any viewer chunk; `RunPhase` counts `heavy` into `published` in every phase (`_heavyThisBeat` reset per phase); `CrowdBody`; items of a map that unloaded between beats are dropped at fill | `BeatPool.h/.cpp` |
| `Player::ViewBuild` bucket loop: `IsPlayer` target in an active crowd cell and `SharedBlockApplies` → `WaitReady`, `Find`, `AppendSharedValuesBlock`; otherwise the per-viewer path | `PlayerUpdates.cpp` |
| `MapUpdate.CrowdBuffer` (`CONFIG_CROWD_BUFFER`, bool, default 0) | `WorldConfig.h/.cpp`, `worldserver.conf.dist` |
| `.debug slowtasks`: `crowd_cells`, `crowd_built`, `crowd_copied`; `.debug memory`: crowd buffers allocated / active / KB, `crowd` items per core | `cs_debug.cpp` |

### Deviations from the design text

- **Per-target blocks, not one blob per cell.** The existing `_valuesUpdateCache` / `PatchValuesUpdate`
  mechanism already splits a values block into shared bytes plus patched offsets (`BuildValuesCachePosPointers`:
  NPC flags, aura state, unit flags, display id, dynamic flags, bytes2, faction, script-tracked). The crowd item
  builds that same cache entry per player with `UF_FLAG_PUBLIC` and a viewer copies the one block of each target
  it actually has in its visible map. Consequences:
  - *Fixed radius* is unnecessary: the viewer filters by its own `VisibleWorldObjectsMap`, so no target is ever
    appended that the viewer does not see, and nothing beyond a radius falls back — there is no radius.
  - *Phase exclusion* is unnecessary for the same reason: a target the viewer cannot see is not in its map.
    The block itself has no phase-dependent bytes.
  - The side table of `(index, offset)` pairs is `BuildValuesCachePosPointers`, already there.
- **Visible-flag exclusions** are exactly the cases in which `BuildValuesUpdateFrom` would widen `visibleFlag`
  beyond `UF_FLAG_PUBLIC`: the viewer is the target (`PRIVATE`), the target's `SUMMONEDBY` is the viewer
  (`OWNER`), `UNIT_DYNFLAG_SPECIALINFO` is set (empathy, per viewer), the target's charmer/owner player is in
  the viewer's raid (`PARTY_MEMBER`, resolved through `GetCharmerOrOwnerPlayerOrPlayerItself(src)` like the
  per-viewer build). Plus stealth / invisibility arrays non-zero on the target slot. All of them take the
  per-viewer path; the crowd item also skips stealthed / invisible targets so their block is never built.
- **No `TaskScope` on the crowd item.** It reads other players' published slots and `GetPackGUID()` /
  `m_valuesCount` / the static flag table — the same foreign reads a viewer chunk makes (step 13 audit) — and
  writes only the cell's `CrowdBuffer`, which no viewer reads before `ready`.
- **Planning is in S2, not "one claim per cell" at run time.** The coordinator plans the cells after every map
  sealed, so there is one item per crowded cell by construction; no claim CAS is needed. A player who moves
  between S2 and the next beat's slot publish is still built from the slot it publishes (the buffer is keyed by
  guid, the viewer looks up by the target's slot `cellId` only to find the buffer — if the target left the cell
  its block is simply not found there and the viewer builds it itself).
- **`ready` is set true on `Deactivate`** and `FindCrowdBuffer` returns null for an inactive buffer, so a viewer
  can only wait on a buffer that has a published item this beat; `RunPhase` asserts every published item ran.

### Owed to the first session that can build

- The gate: a Cell run with `MapUpdate.CrowdBuffer = 0` and `= 1`, `view`, `view_tail`, `wall`,
  `crowd_copied / crowd_built` from `.debug slowtasks`. If `crowd_copied` is small relative to the N² of the
  cell (many raid members in one cell, which is the usual crowd), the step should be dropped and this file is
  the record.
- Visibility regression run: three characters — stealthed, different phase, one tapping a mob — with the
  buffer on. Stealth and phase are excluded by construction; the tap case is `UNIT_DYNAMIC_FLAGS` patched per
  viewer via `PatchValuesUpdate`, as on the per-viewer path, but it is the one row worth seeing.
- Likely compile fixes: `std::erase_if` on the `crowdItems` vector (C++20), `Map const*` from `GetMap()` in
  `ViewBuild`, the `ObjectAccessor::GetPlayer(&map, guid)` overload in `CrowdBuffer::Build`.
