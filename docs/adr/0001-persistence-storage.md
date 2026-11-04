# ADR 0001: Persistence boundary for the prototype

## Status

Accepted for the current prototype; JSON remains the default artifact format and SQLite is an optional adapter when the build discovers sqlite3.

## Decision

The P0-P3 prototype uses JSON Snapshot and JSONL Replay formats behind `IWorldPersistence`. `JsonFilePersistence` writes through a temporary file, flush, and replacement so an interrupted write does not leave a half-written target. An optional `SqlitePersistence` stores the same serialized Snapshot and Replay artifacts in one SQLite file, replacing each named artifact in a transaction and exposing read-back queries for tooling and tests. The explicit `WorldService::saveDurableCommit` operation stores one committed Snapshot plus its Replay artifact as a single checkpoint; SQLite also records identity, Revision, event sequence, idempotency and active-overlay payloads in that transaction. The adapter does not create a second mutable `WorldState` and is not automatically invoked for every in-memory command.

Snapshots and replays are written outside `data/`, preferably under `var/runs/<instance-id>/<run-id>/`. Source content, game-instance definitions and intelligence profiles remain immutable inputs.

## Why SQLite is not the default yet

- The prototype has one in-process writer and needs reproducible, inspectable artifacts more than concurrent queries.
- JSON makes Snapshot/Replay and scenario sandbox inputs easy to diff and reproduce.
- SQLite is therefore opt-in at build time (`TIDE_MARKS_ENABLE_SQLITE`) and JSON remains the inspectable default.
- The user explicitly does not need save compatibility during this unfinished prototype.

## When SQLite becomes necessary

Promote the explicit SQLite checkpoint to the runtime's automatic durable commit path before running long-lived multi-world services or claiming crash-atomic durability for every command. The trigger conditions are concurrent readers/writers, restart-safe idempotency, indexed event queries, incremental event retention, or a requirement that state, events, revision and idempotency records commit in one database transaction.

The automatic path should keep `WorldService` as the semantic authority and use one SQLite transaction for at least:

- current WorldState or a versioned state snapshot;
- append-only WorldEvents, including event/correlation/causation and content/simulation versions;
- world revision and command idempotency records;
- active overlay records and run metadata.

SQLite implements the same persistence boundary, not a second mutable world model. JSON Snapshot/Replay remain import/export and test artifacts. The adapter is injectable at `WorldService` construction, so tests can verify the serialized contract without changing the world rules. The current explicit checkpoint is the first step; automatic per-root-transaction durable commit remains a later phase because it requires a configured run identity and a policy for write latency and crash recovery.
