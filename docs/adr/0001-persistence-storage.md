# ADR 0001: Persistence boundary for the prototype

## Status

Accepted for the current prototype; SQLite is a planned persistence adapter, not a current dependency.

## Decision

The P0-P3 prototype uses the existing JSON Snapshot and JSONL Replay formats behind `IWorldPersistence`. `JsonFilePersistence` writes through a temporary file, flush, and replacement so an interrupted write does not leave a half-written target. The runtime still treats `WorldState` and `EventLog` as one semantic commit boundary; the current file format is a single-process artifact, not a claim of cross-file database atomicity.

Snapshots and replays are written outside `data/`, preferably under `var/runs/<instance-id>/<run-id>/`. Source content, game-instance definitions and intelligence profiles remain immutable inputs.

## Why SQLite is not introduced now

- The prototype has one in-process writer and needs reproducible, inspectable artifacts more than concurrent queries.
- JSON makes Snapshot/Replay and scenario sandbox inputs easy to diff and reproduce.
- Adding SQLite now would add migrations and a second persistence representation before the runtime contracts have settled.
- The user explicitly does not need save compatibility during this unfinished prototype.

## When SQLite becomes necessary

Adopt a SQLite adapter before running long-lived multi-world services or claiming crash-atomic durable commits. The trigger conditions are concurrent readers/writers, restart-safe idempotency, indexed event queries, incremental event retention, or a requirement that state, events, revision and idempotency records commit in one database transaction.

The target adapter should keep `WorldService` as the semantic authority and use one SQLite transaction for at least:

- current WorldState or a versioned state snapshot;
- append-only WorldEvents, including event/correlation/causation and content/simulation versions;
- world revision and command idempotency records;
- active overlay records and run metadata.

SQLite would implement the same persistence boundary, not create a second mutable world model. JSON Snapshot/Replay should remain import/export and test artifacts. The adapter is injectable at `WorldService` construction, so tests can verify the serialized contract without touching disk.
