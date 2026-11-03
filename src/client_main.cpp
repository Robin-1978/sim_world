#include "tide_marks.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#if defined(TIDE_MARKS_HAS_RAYLIB)
#include <raylib.h>
#if defined(TIDE_MARKS_HAS_BULLET)
#include "physics/bullet_physics.hpp"
#endif

namespace {

float clampValue(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float distanceBetween(Vector3 left, Vector3 right) {
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    const float dz = left.z - right.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void drawVillageGeometry() {
    DrawCube({0.0F, -0.35F, 0.0F}, 28.0F, 0.7F, 22.0F, Color{60, 102, 67, 255});
    DrawCube({9.0F, -0.05F, 0.0F}, 6.0F, 0.35F, 22.0F, Color{24, 79, 120, 255});
    DrawCube({0.0F, 0.05F, 0.0F}, 17.0F, 0.16F, 2.0F, Color{110, 111, 104, 255});
    for (const Vector3 house : {Vector3{-6.0F, 1.0F, -5.0F}, Vector3{-1.0F, 1.0F, -5.0F},
                                Vector3{4.0F, 1.0F, -5.0F}, Vector3{-5.0F, 1.0F, 3.0F},
                                Vector3{3.0F, 1.0F, 3.0F}}) {
        DrawCube(house, 3.6F, 2.0F, 3.0F, Color{190, 165, 121, 255});
        DrawCube({house.x, 2.6F, house.z}, 4.5F, 1.1F, 4.0F, Color{38, 66, 98, 255});
    }
    DrawCube({7.8F, 0.65F, 0.0F}, 2.6F, 0.5F, 1.1F, Color{124, 76, 35, 255});
    DrawCube({6.2F, 0.12F, 0.0F}, 4.4F, 0.25F, 2.4F, Color{124, 76, 35, 255});
}

Vector2 mapPoint(Vector3 world, int x, int y, int width, int height) {
    return {static_cast<float>(x) + (world.x + 12.0F) / 24.0F * static_cast<float>(width),
            static_cast<float>(y) + (world.z + 10.0F) / 20.0F * static_cast<float>(height)};
}

void drawVillageMap(Vector3 player, const std::array<std::pair<const char*, Vector3>, 3>& npcs) {
    constexpr int x = 1018;
    constexpr int y = 430;
    constexpr int width = 238;
    constexpr int height = 238;
    DrawRectangle(x, y, width, height, Color{8, 18, 29, 235});
    DrawRectangleLines(x, y, width, height, Color{167, 148, 102, 255});
    DrawText("VILLAGE MAP", x + 10, y + 8, 16, Color{240, 224, 177, 255});
    const auto pathStart = mapPoint({-11.0F, 0.0F, 0.0F}, x, y + 30, width, height - 42);
    const auto pathEnd = mapPoint({8.0F, 0.0F, 0.0F}, x, y + 30, width, height - 42);
    DrawLineEx(pathStart, pathEnd, 4.0F, Color{133, 133, 126, 255});
    const auto waterStart = mapPoint({9.0F, 0.0F, -9.0F}, x, y + 30, width, height - 42);
    const auto waterEnd = mapPoint({9.0F, 0.0F, 9.0F}, x, y + 30, width, height - 42);
    DrawLineEx(waterStart, waterEnd, 18.0F, Color{30, 93, 135, 210});
    for (const auto& [name, position] : npcs) {
        const auto point = mapPoint(position, x, y + 30, width, height - 42);
        const Color color = name == std::string("Mira") ? Color{224, 80, 70, 255} :
                            (name == std::string("Lorn") ? Color{75, 142, 225, 255} : Color{153, 90, 207, 255});
        DrawCircleV(point, 5.0F, color);
        DrawText(name, static_cast<int>(point.x) + 7, static_cast<int>(point.y) - 6, 11, RAYWHITE);
    }
    DrawCircleV(mapPoint(player, x, y + 30, width, height - 42), 5.0F, RAYWHITE);
}

}  // namespace

int main() {
    std::unique_ptr<tide_marks::IProvider> provider = std::make_unique<tide_marks::MockProvider>();
    if (const char* selected = std::getenv("TIDE_MARKS_PROVIDER"); selected != nullptr && std::string(selected) == "llama") {
        tide_marks::LlamaCppConfig config;
        if (const char* model = std::getenv("TIDE_MARKS_MODEL"); model != nullptr) config.model = model;
        if (const char* base = std::getenv("TIDE_MARKS_BASE_URL"); base != nullptr) config.baseUrl = base;
        provider = std::make_unique<tide_marks::LlamaCppProvider>(config);
    }
    auto village = tide_marks::WorldService::load(tide_marks::ContentPackLoader::defaultRoot(), std::move(provider));
    InitWindow(1280, 720, "Tide Marks - Tidemark Village");
    SetTargetFPS(60);
    const bool sceneAssetAvailable = FileExists("assets/blender/village.glb");
    const bool useGlb = sceneAssetAvailable && std::getenv("TIDE_MARKS_USE_GLB") != nullptr;
    Model sceneAsset{};
    if (sceneAssetAvailable) sceneAsset = LoadModel("assets/blender/village.glb");
    Vector3 player{0.0F, 0.65F, 0.0F};
    std::string dialogue = "Walk with WASD. Press E near a marker.";
    float timeAccumulator = 0.0F;
    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        const float speed = 4.0F * dt;
        if (IsKeyDown(KEY_W)) player.z -= speed;
        if (IsKeyDown(KEY_S)) player.z += speed;
        if (IsKeyDown(KEY_A)) player.x -= speed;
        if (IsKeyDown(KEY_D)) player.x += speed;
        player.x = clampValue(player.x, -11.0F, 8.0F);
        player.z = clampValue(player.z, -9.0F, 9.0F);
        timeAccumulator += dt;
        if (timeAccumulator >= 1.0F) {
            village.advanceMinutes(1);
            timeAccumulator = 0.0F;
        }
        const std::array<std::pair<const char*, Vector3>, 3> npcs{{
            {"Mira", {-4.2F, 1.4F, 3.0F}}, {"Lorn", {6.0F, 1.4F, 0.0F}}, {"Eve", {3.0F, 1.4F, 2.2F}}}};
        if (IsKeyPressed(KEY_E)) {
            for (const auto& [name, position] : npcs) {
                if (distanceBetween(player, position) < 2.5F) {
                    const std::string id = name == std::string("Mira") ? "mira" : (name == "Lorn" ? "lorn" : "eve");
                    const auto turn = village.converse(id, "Hello, tell me what is happening.", "client-turn-" + std::to_string(village.state().events.size()));
                    dialogue = turn.accepted ? turn.dialogue : turn.error;
                }
            }
        }
        std::unique_ptr<tide_marks::IPhysicsWorld> physics;
#if defined(TIDE_MARKS_HAS_BULLET)
        physics = tide_marks::makeBulletPhysicsWorld();
#else
        physics = std::make_unique<tide_marks::DeterministicPhysicsWorld>();
#endif
        physics->addBody(tide_marks::PhysicsBody{"player", player.x, player.z, 0.45, true});
        physics->addBody(tide_marks::PhysicsBody{"ferry", 7.8, 0.0, 1.3, false});
        physics->step(0.02);
        const auto physicsEvents = physics->drainEvents();
        if (!physicsEvents.empty()) dialogue = "The ferry rocks as you bump the dock.";

        BeginDrawing();
        ClearBackground(Color{18, 27, 42, 255});
        Camera3D camera{};
        camera.position = {14.0F, 17.0F, 17.0F};
        camera.target = {0.0F, 0.0F, 0.0F};
        camera.up = {0.0F, 1.0F, 0.0F};
        camera.fovy = 45.0F;
        camera.projection = CAMERA_PERSPECTIVE;
        BeginMode3D(camera);
        if (useGlb) {
            DrawModel(sceneAsset, {0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
        } else {
            drawVillageGeometry();
        }
        DrawCube(player, 0.8F, 1.3F, 0.8F, Color{220, 220, 224, 255});
        for (const auto& [name, position] : npcs) {
            const Color color = name == std::string("Mira") ? Color{210, 75, 65, 255} :
                                (name == std::string("Lorn") ? Color{65, 125, 210, 255} : Color{130, 75, 190, 255});
            DrawCube(position, 0.75F, 1.5F, 0.75F, color);
        }
        EndMode3D();
        DrawRectangle(0, 0, 1280, 84, Color{10, 16, 26, 220});
        DrawText("TIDEMARK VILLAGE  |  WASD move  E talk  ESC quit", 24, 16, 22, RAYWHITE);
        DrawText(dialogue.c_str(), 24, 48, 18, Color{224, 210, 170, 255});
        DrawText(TextFormat("%s  grain=%d  ferry=%d  records=%d", village.state().now.toString().c_str(),
                            village.state().village.grain, village.state().village.boatIntegrity,
                            village.state().village.recordsIntegrity), 820, 16, 16, RAYWHITE);
        drawVillageMap(player, npcs);
        EndDrawing();
    }
    if (sceneAssetAvailable) UnloadModel(sceneAsset);
    CloseWindow();
    return 0;
}

#else

// The first client is intentionally dependency-light: it is a deterministic
// scene inspector that can run in CI or over SSH. The raylib/TGUI presentation
// layer will consume the same WorldService/ViewProjection API in the next
// client milestone; no gameplay code depends on a renderer.
int main() {
    auto village = tide_marks::WorldService::load();
    std::cout << "Tide Marks village client (headless scene preview)\n";
    std::cout << "Renderer disabled. Reconfigure build with -DTIDE_MARKS_FETCH_RAYLIB=ON for the 3D window.\n";
    std::cout << "camera=slanted-top-down scene=assets/blender/village.glb\n";
    std::cout << tide_marks::summarize(village.state()) << '\n';
    for (const auto& [id, npc] : village.state().npcs) {
        std::cout << "npc " << npc.definition.name << " role=" << npc.definition.role
                  << " location=" << npc.location << '\n';
    }
    tide_marks::DeterministicPhysicsWorld physics;
    physics.addBody(tide_marks::PhysicsBody{"player", 0.0, 0.0, 0.5, true});
    physics.addBody(tide_marks::PhysicsBody{"ferry", 2.0, 0.0, 1.0, false});
    physics.applyImpulse("player", 1.0, 0.0);
    physics.step(0.02);
    std::cout << "physics_events=" << physics.drainEvents().size() << " (ferry interaction)\n";
    return 0;
}

#endif
