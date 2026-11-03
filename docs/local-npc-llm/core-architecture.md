# Core Runtime Architecture

## Entity, component, and asset boundaries

The runtime uses an Entity + Component direction for interactive objects, while authored content remains data-driven. An entity is a stable `entity_id`; components hold facts such as transform, location, render asset, physics body, inventory, schedule, door state, room membership, or NPC identity. Systems own behavior and rules. Components do not call the world or mutate each other.

| Layer | Example | Lifetime | Authority |
|---|---|---|---|
| Authored asset | `data/npcs/mira.json`, dialogue beat, Blender mesh | versioned content | content loader/schema |
| Entity | `npc:mira`, `door:granary`, `room:archive` | save slot | `WorldService` |
| Component | `NpcComponent`, `DoorComponent`, `TransformComponent` | save slot | owning system through commands |
| Presentation | `assets/blender/village.glb`, portrait, audio | replaceable files | client/view |

P0 keeps `NpcState` as a compact compatibility struct so the slice is easy to test. It is the serialized façade over the future component stores, not a permission for NPC code to mutate arbitrary fields. P1 will introduce an `EntityRegistry` and migrate NPC, door, room, and item state behind the same command API. Content assets are never the live state: changing Mira's JSON definition does not silently rewrite a save.

NPC assets include identity, role, goals, boundaries, voice style, schedule, portrait/model IDs, dialogue beats, and allowed actions. They do not contain private runtime memories, current relationships, promises, or API keys; those belong to the save state.

## World state and rule validation

Every mutation follows:

```text
Controller / offline scheduler
  -> Command schema and ID validation
  -> authorization and visibility checks
  -> world/NPC revision and idempotency checks
  -> rule preconditions (time, location, resources, relationship, promise)
  -> physics adapter checks, when spatial
  -> atomic state mutation
  -> WorldEvent append + EventBus publish
  -> projection, memory update, save/replay output
```

`WorldState` is the only fact authority. LLM output, behavior-tree nodes, renderers, and event subscribers cannot write it directly. A failed validation returns a reason and leaves the state and revision unchanged.

## State machine, utility, and behavior tree

- **Utility/GoalSelector** ranks long-lived goals from pressure, resources, promises, time, and relationships.
- **State machine** owns coarse modes such as `AtHome`, `Working`, `InConversation`, `Traveling`, `Interrupted`, and `Unavailable`.
- **Behavior tree** executes the selected short plan with conditions, actions, timeout, cooldown, sequence/selector, and interrupt nodes.
- **LLM** chooses or expresses only an authored candidate; it cannot create a component, transition, room, item, or event.

## Communication channels

`EventBus` is the in-process notification channel for committed facts. It is not the only communication path and it is not a network bus:

- Commands enter through `WorldService::submit()` or dialogue APIs.
- Queries return read-only `WorldState`/`ViewProjection` data.
- `EventBus` delivers ordered notifications to memory, quests, UI, telemetry, and debug subscribers.
- `IProvider` is the isolated LLM boundary.
- `IPhysicsWorld` is the isolated physics boundary.
- Event log/replay is the persistence boundary.

The bus supports exact event types and `"*"`, queues FIFO, dispatches synchronously at the commit boundary, and allows subscribers to enqueue later events without reordering the current event.

## Save and debugging

Snapshots store content version, game time, entities/components, revisions, promises, memories, and processed turn IDs. JSONL replay stores the snapshot plus ordered `WorldEvent` records; replay never calls the LLM. Use `tide_marks_headless`, CTest, `/tmp` replay files, and event subscriptions as the current debug tools. A developer console and live event inspector are P1 tools.
