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

- [ ] `CrowdBuffer` item, one claim per cell, first in U2 via the heavy ring, per-cell `ready` flag.
- [ ] Players only, fixed radius, dynamic indices patched per viewer.
- [ ] Stealth / phase / private exclusions.
- [ ] Gate numbers in the exit note, or the step dropped with the numbers that justified dropping it.
