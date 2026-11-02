# Third-party notices

The project keeps dependency selection in CMake and does not rely on globally installed headers.

- **nlohmann/json 3.11.3** — MIT License. The pinned source is under `third_party/nlohmann_json/` and is consumed by `FetchContent`.
- **raylib 5.5** — zlib/libpng License. It is fetched by CMake when `TIDE_MARKS_FETCH_RAYLIB=ON`.
- **Bullet 3.25** — zlib/libpng License. It is fetched by CMake when `TIDE_MARKS_FETCH_BULLET=ON`; the game-facing interface remains `IPhysicsWorld`.
- **Blender 5.2.2** — Blender Foundation license for the tool; the generated low-poly scene in `assets/blender/` is project content produced by `scripts/create_village_scene.py`.

Model weights are not distributed by this repository. Users must obtain a compatible local model and comply with its own license. API keys are read from environment variables and are never stored in content, saves, or replay files.
