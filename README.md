# 《潮痕》 Tide Marks

The repository currently contains the first playable three-NPC village slice. Run all commands from the repository root.

Tide Marks is a rule-driven world that keeps living without the player at its center. Local language models interpret events, reason about social situations, and propose constrained changes; deterministic transactions, events, saves, and validators decide what becomes true. World evolution and capability evolution must stay observable, reproducible, and reversible.

## Headless run

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --days 2 --provider mock --replay /tmp/tide_marks-village.jsonl
./build/tide_marks_headless --days 2 --provider mock --dump-events
```

The explicit `mock` provider makes this command deterministic and offline. It prints the starting state, a dialogue turn, two days of NPC life, and the replay summary. It does not require a display or a running model server. `./build/tide_marks_client` is a display-free scene inspector.

The headless entry point reads the world identity, first dialogue participant, content version, and opening state from the selected pack. Load a different instance, including a genuinely different fiction, with:

```bash
./build/tide_marks_headless \
  --content-root tests/fixtures/glass-orchard-pack \
  --days 1 --provider mock --dump-events
```

## Text interface

For core gameplay without graphics, use the terminal client:

```bash
./build/tide_marks_text
```

Try `look`, `map`, `entities`, `talk mira ration please`, `day`, `events`, `save /tmp/village.json`, and `quit`. The text console also exposes the lifecycle path with `entity-create <id> <archetype>`, `entity-dormant <id>`, `entity-activate <id>`, and `entity-destroy <id>`. Exchange contracts can be exercised with `exchange <from> <to> <minutes> <offered-json> <requested-json>`, followed by `exchange-accept <id> <actor>` and `exchange-settle <id> <actor>`; terms move only at settlement. Use `look` for live contract state or `contracts [count]` for the full term and lifecycle record. Scripted sessions are also supported:

```bash
./build/tide_marks_text --script docs/examples/village-session.txt
```

Pass `--content-root` to inspect another loaded instance with the same text console:

```bash
./build/tide_marks_text \
  --content-root tests/fixtures/glass-orchard-pack \
  --script tests/fixtures/glass-orchard-pack/session.txt \
  --provider mock
```

## 3D window

raylib is fetched and built by CMake; no system raylib installation is used. Keep the same `build/` directory:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTIDE_MARKS_FETCH_RAYLIB=ON
cmake --build build -j2
./build/tide_marks_client
```

If GitHub is unreachable, set `HTTPS_PROXY` to a proxy configured for your environment for the configure command. **WASD** moves, **E** talks to a nearby NPC, and **Esc** exits. A content pack may declare an optional `scene_asset` relative to its content root and a supported `camera` mode (`slanted_top_down` or `top_down`). The procedural spatial view remains the fallback when no asset is configured.

The raylib view draws authored spatial entities and links from the selected content pack. If that pack declares a scene asset and `TIDE_MARKS_USE_GLB=1` is set, the client previews the asset instead of the procedural geometry. The map bounds, regions, links, NPC markers, and resource HUD are all derived from the loaded world.

The client, text UI, and headless runner use the checked-in intelligence profile by default:

```bash
./build/tide_marks_client
./build/tide_marks_text
./build/tide_marks_headless --days 0
```

Pass `--provider mock` only for deterministic offline tests. Conversation with the mock provider is an explicit test mode and does not represent the production LLM path.

To observe the real local model making three independent self-drive decisions at one world
boundary, run:

```bash
./build/tide_marks_text --provider profile --script docs/examples/llm-self-drive-session.txt
```

The `drain` command is an explicit bounded wait for queued Provider work. It is useful for a
headless smoke test; an interactive loop should keep calling `pumpAsyncTurns()` at world
boundaries instead of blocking the simulation.

The scene inspector derives NPC markers and resource HUD values from the selected content pack. Another instance therefore does not inherit village actors or named resource fields:

```bash
./build/tide_marks_client --content-root tests/fixtures/glass-orchard-pack --provider mock
```

Bullet 3.25 is an independent CMake option for the production physics adapter:

```bash
cmake -S . -B build -G Ninja -DTIDE_MARKS_FETCH_BULLET=ON
cmake --build build -j2
```

## Local LLM

`data/intelligence-profile.json` selects the local llama.cpp endpoint, model, and per-decision token policy. With llama.cpp serving OpenAI-compatible HTTP on `127.0.0.1:8192`:

```bash
./build/tide_marks_headless --profile data/intelligence-profile.json --provider profile --days 0
```

Player dialogue uses two model stages: semantic utterance understanding with a bounded thinking budget, then a fast answer/action decision. Autonomous reappraisal, which weighs drives and external stimuli without a user turn, uses a short reasoning budget.

The LLM interprets player text and selects authored actions. Runtime validates every proposal and owns all facts. If the LLM is unavailable or returns invalid JSON, a player conversation fails closed without an event or world change; Runtime never guesses intent from keywords.

To exercise governed Level 1 and Level 2 evolution with the real model, run:

```bash
./build/tide_marks_headless --profile data/intelligence-profile.json \
  --provider profile --days 0 --evolve level1
./build/tide_marks_headless --profile data/intelligence-profile.json \
  --provider profile --days 0 --evolve level2
```

These commands print the LLM proposal only. Runtime validation, sandbox execution, independent approval, and versioned overlay activation are separate steps; requesting a proposal never mutates the source world. For deterministic schema and governance tests, use the repository's CTest suite, which injects `MockProvider` explicitly.

The village content pack also enables automatic, asynchronous evolution proposals at configured day boundaries. This path does not block a world tick and does not self-approve an overlay. To observe a bounded Level 1 proposal while advancing an interactive-style day, use `--wait-llm-ms` (the local model may still take several seconds per queued request):

```bash
./build/tide_marks_headless --profile data/intelligence-profile.json \
  --provider profile --days 1 --wait-llm-ms 30000 --dump-events
```

The resulting `evolution.proposal_ready` event contains the validated proposal and deterministic sandbox result. A developer or world designer must approve and activate it through the normal overlay governance path.

An explicit durable checkpoint can be requested from the C++ API with `WorldService::saveDurableCommit(path)`. JSON remains the default inspectable artifact; when SQLite is discovered at configure time, `SqlitePersistence` stores the checkpoint transactionally. This checkpoint operation is explicit and does not write the database on every simulation minute.

The project goal is summarized in [`docs/goal.md`](docs/goal.md); architecture and continuation checkpoints are in [`docs/design.md`](docs/design.md) and [`docs/release-continuation-plan.md`](docs/release-continuation-plan.md).

## World Intelligence checks and counterfactuals

Anomaly rules are declared by a content pack and can be inspected without invoking an LLM:

```bash
./build/tide_marks_headless --content-root tests/fixtures/port-pack --anomalies --days 0
```

Scenario proposals are isolated runs from the current snapshot. They never mutate the source world and require an exact `base_world_revision` and `base_content_version`:

```bash
./build/tide_marks_headless \
  --content-root tests/fixtures/port-pack \
  --experiment docs/examples/port-morale-scenario.json
```

The command prints the applied levers, baseline/scenario metrics, numeric deltas, generated events, and the resulting snapshot as JSON. A proposal can only change facts for which the loaded content pack already declares a bounded intervention; missing economics or policy mechanisms are rejected instead of being invented by the model.

## Runtime boundary and data layout

The runtime flow is deliberately split into four boundaries:

```text
visible facts + needs + stimuli + memory
                    |
                    v
              LLM DecisionContext
                    |
                    v
  speech act / IntentProposal / evolution proposal
                    |
                    v
      schema + permission + revision validation
                    |
                    v
  NpcActivity -> ProcessExecution -> BT -> operators
                    |
                    v
       atomic WorldState commit + WorldEvent
                    |
                    v
       perception, memory, relationships, drives
```

`Goal`, `ProcessExecution`, FSM and BT are four views of one `NpcActivity`; one accepted decision
can run for many ticks and is re-evaluated at completion, interruption or a configured salient
boundary. A conversation may produce only speech. A request or command may propose a world action,
but it still goes through the same Runtime validation path. LLM proposals cannot call arbitrary
code or write facts directly.

`data/` is immutable Content Pack input: world rules, NPC archetypes, behavior recipes, dialogue,
instances, schemas and the Intelligence Profile. It is never a save directory. Snapshots, Replay
packages, evolution proposals and sandbox reports belong outside it, for example:
`var/runs/<instance-id>/<run-id>/`. See [`data/README.md`](data/README.md) for the full split.

An explicit durable checkpoint can be configured without an environment variable. The default is
no automatic durable write:

```bash
./build/tide_marks_headless --provider mock --days 2 \
  --durable-checkpoint var/runs/village/run-1/checkpoint.json
./build/tide_marks_headless --provider mock --days 2 \
  --durable-checkpoint var/runs/village/run-1/checkpoint.json \
  --checkpoint-every-minutes 60
```

JSON is the inspectable default; SQLite is a statically compiled adapter for the same committed
Snapshot/Replay boundary. CMake downloads the pinned SQLite amalgamation, so deployment does not
require a system sqlite3 package. Replay is an explicit verification artifact, not per-tick
persistence and not full Event Sourcing.
