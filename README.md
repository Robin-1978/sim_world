# 《潮痕》 Tide Marks

The repository currently contains the first playable three-NPC village slice. Run all commands from the repository root.

Tide Marks is a rule-driven world that keeps living without the player at its center. Local language models interpret events, reason about social situations, and propose constrained changes; deterministic transactions, events, saves, and validators decide what becomes true. World evolution and capability evolution must stay observable, reproducible, and reversible.

## Headless run

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --days 2 --replay /tmp/tide_marks-village.jsonl
./build/tide_marks_headless --days 2 --dump-events
```

This prints the starting state, a dialogue turn, two days of NPC life, and the replay summary. It does not require a display or a running model server. `./build/tide_marks_client` is a display-free scene inspector.

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

Try `look`, `map`, `entities`, `talk mira ration please`, `day`, `events`, `save /tmp/village.json`, and `quit`. The text console also exposes the lifecycle path with `entity-create <id> <archetype>`, `entity-dormant <id>`, `entity-activate <id>`, and `entity-destroy <id>`. Scripted sessions are also supported:

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

If GitHub is unreachable, set `HTTPS_PROXY` to a proxy configured for your environment for the configure command. The window uses a slanted 3D camera. **WASD** moves, **E** talks to a nearby NPC, and **Esc** exits. The client loads `assets/blender/village.glb` when started from the repository root.

The default raylib view uses a colored procedural village layout and shows a village minimap. Set `TIDE_MARKS_USE_GLB=1` to preview the exported Blender mesh instead. The current world map covers the P0 village; the full island map is planned for the P1/P2 world expansion.

To use the running local model in the 3D client, select the checked-in intelligence profile:

```bash
./build/tide_marks_client --profile data/intelligence-profile.json --provider profile
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

Player dialogue is configured with thinking off. Autonomous reappraisal, which weighs drives and external stimuli without a user turn, uses a short reasoning budget. Use `--provider mock` for deterministic offline runs.

The provider only selects authored actions. If the server times out or returns invalid JSON, the game uses deterministic fallback dialogue.

The design and continuation checkpoint are in [`docs/design.md`](docs/design.md) and [`docs/release-continuation-plan.md`](docs/release-continuation-plan.md).

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
