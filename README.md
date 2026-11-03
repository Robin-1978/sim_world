# 《潮痕》 Tide Marks

The repository currently contains the first playable three-NPC village slice. Run all commands from the repository root.

## Headless run

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --scenario village --days 2 --replay /tmp/tide_marks-village.jsonl
```

This prints the starting state, a dialogue turn, two days of NPC life, and the replay summary. It does not require a display or a running model server. `./build/tide_marks_client` is a display-free scene inspector.

## 3D window

raylib is fetched and built by CMake; no system raylib installation is used. Keep the same `build/` directory:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTIDE_MARKS_FETCH_RAYLIB=ON
cmake --build build -j2
./build/tide_marks_client
```

If GitHub is unreachable, prefix the configure command with `HTTPS_PROXY=http://gfw.gs-robot.cn:6780`. The window uses a slanted 3D camera. **WASD** moves, **E** talks to a nearby NPC, and **Esc** exits. The client loads `assets/blender/village.glb` when started from the repository root.

Bullet 3.25 is an independent CMake option for the production physics adapter:

```bash
cmake -S . -B build -G Ninja -DTIDE_MARKS_FETCH_BULLET=ON
cmake --build build -j2
```

## Local LLM

With llama.cpp serving OpenAI-compatible HTTP on `127.0.0.1:8192`:

```bash
TIDE_MARKS_MODEL=Huihui-Qwen3.5-4B-abliterated.Q4_K_M \
  ./build/tide_marks_headless --provider llama --days 0
```

The provider only selects authored actions. If the server times out or returns invalid JSON, the game uses deterministic fallback dialogue.

The design and continuation checkpoint are in [`docs/release-continuation-plan.md`](docs/release-continuation-plan.md).
