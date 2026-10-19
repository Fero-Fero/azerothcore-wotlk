Still open

These were left unfinished on purpose, and a logged-in client is what they need:

Spell-hit slice 4. Land-side scripts still see a live caster. The plan is to change that type and fix each compiler error as a slot read, a feedback record, or a deferred row. That pass is a behavior change, not a mechanical rename.
Grid tasks (S1a), spawn defers from respawns, and moving every free to the serial seal.
Visibility link records stay off. An object removed at the end of a tick can be freed before the record is applied.
Module database pools have no sync-query guard. The main pools do, and in a debug build a sync query from a unit task is still meant to assert.
Plans 13–17's play checks: foreign-access asserts, the worker soak, memory numbers, and the crowd-buffer on/off comparison. MapUpdate.CrowdBuffer remains off until that comparison exists.
