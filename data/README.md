# Data layout

`data/` is the immutable input tree for a Content Pack and its Intelligence Profile. It is not a save directory and must never contain a live `WorldState`, an EventLog from a run, or a replay artifact.

Creating a game instance imports the JSON documents into one SQLite database. The
database then carries an immutable content-document bundle alongside the mutable
instance facts, so `WorldService::loadSqliteInstance()` does not need the source
JSON directory. The original files remain the authoring/template source for
creating another instance or publishing a new content version.

## What belongs here

| Path | Role | Mutable during a run? |
|---|---|---|
| `content-pack.json` | Content Pack manifest; points to the world, NPC, behavior, dialogue and instance definitions | No |
| `world/*.json` | World rules, topology, resources, policies, schedules and static defaults | No |
| `npcs/*.json` | NPC archetypes, needs, authored identity, private traits, baseline schedules and candidate actions | No |
| `behaviors/*.json` | Behavior recipes, conditions and meta-action bindings | No |
| `dialogue/*.json` | Dialogue beats, speech permissions, authored communication vocabulary and action `trait_effects` | No |
| `instances/*.json` | Game Instance opening configuration: identity, initial time, placement and seeded inventory | No |
| `intelligence-profile.json` | Sample Provider/model and decision-channel policy; installed deployments may copy it to `~/.sim_world/config/` | No |
| `schemas/*.schema.json` | JSON contracts for content, proposals, events and snapshots | No |

The files under `world/`, `npcs/`, `behaviors/`, `dialogue/` and `instances/` together form a reusable static input. `instances/` does not mean a running save: it is the authored opening state used to create one run.

## What does not belong here

When a world is running, one SQLite file owns the instance checkpoint: the
imported immutable content documents plus `WorldService`'s mutable `WorldState`
(current resources, inventories, relationships, needs, activity/process state,
entity lifecycle, active overlays, revision and committed events). A Snapshot
serializes the mutable state; a Replay packages a Snapshot and its event history.
The JSON file adapter remains useful for inspection and tests, but a product run
should use one database per instance in an external run directory such as
`~/.sim_world/instances/<instance-id>/world.sqlite`.

Evolution proposals, sandbox results and scenario reports are also run or governance artifacts. Keep them beside the run that produced them, with the content and simulation versions recorded inside the artifact. Never edit a source file under `data/` to represent a runtime change; an approved content change creates a new content version or overlay.

The distinction is therefore:

```text
data/ static authoring templates + schemas + deployment profile
       -- new game -->  instance.sqlite
                         ├── imported content documents (read-only)
                         └── WorldState + EventLog + overlays (mutable)
```

An existing database keeps `content_version`, `instance_id`, `definition_id`
references, active overlays and event provenance. A new game is a new database;
re-importing a template never overwrites an existing instance. If an imported
content bundle or runtime definition no longer matches those identities,
restore is rejected instead of silently mixing new definitions with old facts.
The content bundle is the only JSON input that becomes part of the instance:
the deployment's Intelligence Profile (and its API-key environment-variable
name), JSON Schemas, visual assets and compiled Runtime operators stay outside
the save because they are deployment/configuration or code, not mutable world
facts.
