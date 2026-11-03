# Repository Guidelines

## Project Structure & Module Organization

This is the C++20 core for 《潮痕》, with a deterministic three-NPC village slice.

- `src/core/` — time, entity identity, events, snapshots, and JSON types.
- `src/world/` — authoritative `WorldService`, generic components, schedules, promises, progression, and replay.
- `src/content/` — versioned JSON content-pack loading and validation. Content names are data; the runtime must not hardcode a specific game's rooms, doors, NPC IDs, or story.
- `src/npc/` — behavior-tree nodes (`Sequence`, `Selector`, `Condition`, `Action`, `Cooldown`, `Timeout`, `Interruptible`).
- `src/provider/` — Mock and OpenAI-compatible llama.cpp providers; the local default is `http://127.0.0.1:8192/v1`.
- `src/*_main.cpp` — headless scenario runner, text UI, and dependency-light scene inspector.
- `tests/` — deterministic core and village acceptance tests.
- `data/` — authored world, NPC, dialogue, and schema content.
- `assets/blender/` — generated low-poly village `.blend`/`.glb`; regenerate with `scripts/create_village_scene.py`.
- `docs/` — architecture, development, and continuation plans.
- `third_party/` — pinned sources consumed through CMake `FetchContent`; do not replace them with system headers.

Keep all build output in the single `build/` directory. Do not commit replay files, caches, credentials, or generated logs.

This project is pre-release. Do not add compatibility aliases, legacy parsing paths, or migration shims for designs that have not shipped. When the data model changes, update the single current schema, content pack, code, tests, and documentation together.

## Build, Test, and Development Commands

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build --output-on-failure
cmake -S . -B build-gcc13 -G Ninja -DCMAKE_CXX_COMPILER=g++-13
cmake --build build-gcc13 -j2
./build-gcc13/tide_marks_headless --days 2 --replay /tmp/tide_marks-village.jsonl
# Optional renderer/physics build (FetchContent, no system raylib/Bullet)
cmake -S . -B build -G Ninja -DTIDE_MARKS_FETCH_RAYLIB=ON -DTIDE_MARKS_FETCH_BULLET=ON
cmake --build build -j2
./build/tide_marks_text --script docs/examples/village-session.txt
```

The first three commands are the canonical build; the GCC 13 variant is useful for release validation. The headless runner must remain usable without a display or a running LLM server. Use `--provider llama` to exercise the local OpenAI-compatible endpoint; failures must fall back to authored/mock dialogue.

Use `--dump-events` to inspect the committed event stream while debugging a scenario.

## Coding Style & Naming Conventions

Use C++20, four-space indentation, `PascalCase` types, `camelCase` members/functions, and `snake_case` JSON keys. Keep gameplay mutations inside `WorldService`; providers return candidates and text only. Format changes should be narrow and warning-clean under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.

## Testing Guidelines

Add a deterministic test for every world rule, provider validation path, promise transition, transaction boundary, component projection, and replay invariant. Run CTest from the build directory and keep tests independent of network, graphics, wall-clock time, or model availability.

## Commit & Pull Request Guidelines

Use concise imperative commits such as `Add deterministic village slice`. PRs should describe behavior, content version, validation commands/results, and any provider or asset changes. Include screenshots only when a rendered client changes; headless output is sufficient for core changes.
