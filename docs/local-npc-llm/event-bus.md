# Event Bus Contract

`WorldEvent` is the canonical fact envelope. It has a schema version, monotonic sequence, game time, world revision, type, actor, target, human-readable detail, and structured JSON data.

The event log is durable history. `EventBus` is the in-process delivery mechanism for the same committed event. `WorldService::addEvent()` appends first, then queues and dispatches the event. A subscriber can observe a fact but cannot veto or mutate the commit.

Subscriptions are exact type strings such as `life.day`, `dialogue.turn`, or `promise.broken`, or the wildcard `*`. Delivery order is subscription-token order within FIFO event order. Publishing while dispatching appends to the queue and is delivered after the current event. Replay reads the durable log and does not invoke live providers.

Typical consumers are:

- `MemoryService`: turns visible events into confirmed or candidate memories.
- `QuestService`: advances authored beats after validated events.
- `ViewProjection`: refreshes UI-visible state.
- `Telemetry/Debug`: counts transitions and displays event traces.
- `SaveService`: checkpoints after a configured event boundary.

Commands, queries, provider calls, physics results, and persistence remain separate interfaces. This prevents the event bus from becoming a hidden global write channel.
