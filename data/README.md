# Data layout

`data/` is the immutable input tree for a Content Pack and its Intelligence Profile. It is not a save directory and must never contain a live `WorldState`, an EventLog from a run, or a replay artifact.

## What belongs here

| Path | Role | Mutable during a run? |
|---|---|---|
| `content-pack.json` | Content Pack manifest; points to the world, NPC, behavior, dialogue and instance definitions | No |
| `world/*.json` | World rules, topology, resources, policies, schedules and static defaults | No |
| `npcs/*.json` | NPC archetypes, needs, authored identity, private traits, baseline schedules and candidate actions | No |
| `behaviors/*.json` | Behavior recipes, conditions and meta-action bindings | No |
| `dialogue/*.json` | Dialogue beats, speech permissions, authored communication vocabulary and action `trait_effects` | No |
| `instances/*.json` | Game Instance opening configuration: identity, initial time, placement and seeded inventory | No |
| `intelligence-profile.json` | Provider/model and decision-channel policy; it is deployment configuration, not world state | No |
| `schemas/*.schema.json` | JSON contracts for content, proposals, events and snapshots | No |

The files under `world/`, `npcs/`, `behaviors/`, `dialogue/` and `instances/` together form a reusable static input. `instances/` does not mean a running save: it is the authored opening state used to create one run.

## What does not belong here

When a world is running, `WorldService` owns the mutable `WorldState`: current resources, inventories, relationships, needs, activity/process state, entity lifecycle, active overlays, revision and committed events. A Snapshot serializes that state; a Replay packages a Snapshot and its event history. These are run artifacts and should be written to an external run directory such as `var/runs/<instance-id>/<run-id>/` or an explicitly supplied path.

Evolution proposals, sandbox results and scenario reports are also run or governance artifacts. Keep them beside the run that produced them, with the content and simulation versions recorded inside the artifact. Never edit a source file under `data/` to represent a runtime change; an approved content change creates a new content version or overlay.

The distinction is therefore:

```text
data/ static definitions + schemas + deployment profile
       -> WorldService creates
WorldState current facts + EventLog committed history
       -> snapshot/replay/export outside data/
```

An existing Snapshot keeps `content_version`, `instance_id`, `definition_id` references, active overlays and event provenance. If the source definitions no longer match those identities, restore is rejected instead of silently mixing a new definition with old facts.
