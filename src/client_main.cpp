#include "tide_marks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

struct WorldBounds {
    float minX{-10.0F};
    float maxX{10.0F};
    float minZ{-10.0F};
    float maxZ{10.0F};
};

struct NpcMarker {
    std::string id;
    std::string name;
    Vector3 position{};
};

WorldBounds worldBounds(const tide_marks::WorldService& world) {
    WorldBounds result;
    if (world.content().spatialEntities.empty()) return result;
    result.minX = std::numeric_limits<float>::max();
    result.maxX = std::numeric_limits<float>::lowest();
    result.minZ = std::numeric_limits<float>::max();
    result.maxZ = std::numeric_limits<float>::lowest();
    for (const auto& entity : world.content().spatialEntities) {
        result.minX = std::min(result.minX, static_cast<float>(entity.spatial.minX));
        result.maxX = std::max(result.maxX, static_cast<float>(entity.spatial.maxX));
        result.minZ = std::min(result.minZ, static_cast<float>(entity.spatial.minZ));
        result.maxZ = std::max(result.maxZ, static_cast<float>(entity.spatial.maxZ));
    }
    const float width = std::max(result.maxX - result.minX, 1.0F);
    const float depth = std::max(result.maxZ - result.minZ, 1.0F);
    const float padding = std::max(width, depth) * 0.18F;
    result.minX -= padding;
    result.maxX += padding;
    result.minZ -= padding;
    result.maxZ += padding;
    return result;
}

Vector3 cameraPosition(const std::string& mode, const WorldBounds& bounds) {
    const float centerX = (bounds.minX + bounds.maxX) * 0.5F;
    const float centerZ = (bounds.minZ + bounds.maxZ) * 0.5F;
    const float extent = std::max(bounds.maxX - bounds.minX, bounds.maxZ - bounds.minZ);
    if (mode == "top_down") {
        return {centerX, std::max(12.0F, extent * 1.4F), centerZ + 0.01F};
    }
    return {centerX + extent * 0.95F, std::max(10.0F, extent * 1.25F), centerZ + extent * 0.95F};
}

Vector3 spatialCenter(const tide_marks::SpatialContent& entity) {
    return {static_cast<float>((entity.spatial.minX + entity.spatial.maxX) * 0.5), 0.0F,
            static_cast<float>((entity.spatial.minZ + entity.spatial.maxZ) * 0.5)};
}

Color stableColor(const std::string& value, unsigned char alpha = 255) {
    const auto hash = std::hash<std::string>{}(value);
    return Color{static_cast<unsigned char>(70U + (hash & 0x7FU)),
                 static_cast<unsigned char>(80U + ((hash >> 8U) & 0x7FU)),
                 static_cast<unsigned char>(95U + ((hash >> 16U) & 0x7FU)), alpha};
}

void drawWorldGeometry(const tide_marks::WorldService& world) {
    // Geometry is a view of authored spatial entities. A content pack can
    // replace this fallback with its own scene asset without changing Runtime.
    for (const auto& link : world.content().links) {
        if (link.link.endpoints.size() < 2) continue;
        const auto findEntity = [&world](const std::string& id) {
            return std::find_if(world.content().spatialEntities.begin(), world.content().spatialEntities.end(),
                                [&id](const auto& entity) { return entity.id == id; });
        };
        const auto first = findEntity(link.link.endpoints[0]);
        const auto second = findEntity(link.link.endpoints[1]);
        if (first == world.content().spatialEntities.end() || second == world.content().spatialEntities.end()) continue;
        auto start = spatialCenter(*first);
        auto end = spatialCenter(*second);
        start.y = 0.18F;
        end.y = 0.18F;
        DrawLine3D(start, end, link.link.traversable ? Color{150, 150, 145, 230} : Color{90, 65, 65, 230});
    }
    for (const auto& entity : world.content().spatialEntities) {
        const auto center = spatialCenter(entity);
        const auto width = static_cast<float>(std::max(entity.spatial.maxX - entity.spatial.minX, 0.8));
        const auto depth = static_cast<float>(std::max(entity.spatial.maxZ - entity.spatial.minZ, 0.8));
        DrawCube({center.x, 0.0F, center.z}, width, 0.3F, depth, stableColor(entity.kind));
        DrawCubeWires({center.x, 0.17F, center.z}, width, 0.04F, depth, Color{210, 195, 160, 220});
    }
}

Vector2 mapPoint(Vector3 world, const WorldBounds& bounds, int x, int y, int width, int height) {
    const float worldWidth = std::max(bounds.maxX - bounds.minX, 1.0F);
    const float worldDepth = std::max(bounds.maxZ - bounds.minZ, 1.0F);
    return {static_cast<float>(x) + (world.x - bounds.minX) / worldWidth * static_cast<float>(width),
            static_cast<float>(y) + (world.z - bounds.minZ) / worldDepth * static_cast<float>(height)};
}

void drawWorldMap(const tide_marks::WorldService& world, Vector3 player,
                  const std::vector<NpcMarker>& npcs, const WorldBounds& bounds) {
    constexpr int x = 1018;
    constexpr int y = 430;
    constexpr int width = 238;
    constexpr int height = 238;
    DrawRectangle(x, y, width, height, Color{8, 18, 29, 235});
    DrawRectangleLines(x, y, width, height, Color{167, 148, 102, 255});
    DrawText("WORLD MAP", x + 10, y + 8, 16, Color{240, 224, 177, 255});
    for (const auto& entity : world.content().spatialEntities) {
        const auto topLeft = mapPoint({static_cast<float>(entity.spatial.minX), 0.0F,
                                       static_cast<float>(entity.spatial.minZ)}, bounds, x, y + 30, width, height - 42);
        const auto bottomRight = mapPoint({static_cast<float>(entity.spatial.maxX), 0.0F,
                                           static_cast<float>(entity.spatial.maxZ)}, bounds, x, y + 30, width, height - 42);
        DrawRectangle(static_cast<int>(topLeft.x), static_cast<int>(topLeft.y),
                      std::max(2, static_cast<int>(bottomRight.x - topLeft.x)),
                      std::max(2, static_cast<int>(bottomRight.y - topLeft.y)), stableColor(entity.kind, 175));
    }
    for (const auto& link : world.content().links) {
        if (link.link.endpoints.size() < 2) continue;
        const auto findEntity = [&world](const std::string& id) {
            return std::find_if(world.content().spatialEntities.begin(), world.content().spatialEntities.end(),
                                [&id](const auto& entity) { return entity.id == id; });
        };
        const auto first = findEntity(link.link.endpoints[0]);
        const auto second = findEntity(link.link.endpoints[1]);
        if (first == world.content().spatialEntities.end() || second == world.content().spatialEntities.end()) continue;
        const auto start = mapPoint(spatialCenter(*first), bounds, x, y + 30, width, height - 42);
        const auto end = mapPoint(spatialCenter(*second), bounds, x, y + 30, width, height - 42);
        DrawLineEx(start, end, 2.0F, link.link.traversable ? Color{188, 188, 176, 255} : Color{112, 76, 76, 255});
    }
    for (const auto& marker : npcs) {
        const auto point = mapPoint(marker.position, bounds, x, y + 30, width, height - 42);
        const Color color = stableColor(marker.id);
        DrawCircleV(point, 5.0F, color);
        DrawText(marker.name.c_str(), static_cast<int>(point.x) + 7, static_cast<int>(point.y) - 6, 11, RAYWHITE);
    }
    DrawCircleV(mapPoint(player, bounds, x, y + 30, width, height - 42), 5.0F, RAYWHITE);
}

std::vector<NpcMarker> npcMarkers(const tide_marks::WorldService& world) {
    std::vector<NpcMarker> markers;
    for (const auto& authored : world.content().npcs) {
        const auto state = world.state().npcs.find(authored.definition.id);
        if (state == world.state().npcs.end()) continue;
        const auto spatial = std::find_if(world.content().spatialEntities.begin(),
                                          world.content().spatialEntities.end(),
                                          [&state](const auto& item) { return item.id == state->second.location; });
        if (spatial == world.content().spatialEntities.end()) continue;
        const auto& bounds = spatial->spatial;
        markers.push_back(NpcMarker{authored.definition.id, authored.definition.name,
                                    Vector3{static_cast<float>((bounds.minX + bounds.maxX) * 0.5), 1.4F,
                                             static_cast<float>((bounds.minZ + bounds.maxZ) * 0.5)}});
    }
    return markers;
}

std::string resourceSummary(const tide_marks::WorldService& world) {
    std::ostringstream output;
    std::size_t shown = 0;
    for (const auto& [id, resource] : world.state().resources) {
        const auto item = id + "=" + std::to_string(resource.amount);
        const auto separator = shown == 0 ? std::string{} : std::string{"  "};
        if (output.str().size() + separator.size() + item.size() > 54) break;
        output << separator << item;
        ++shown;
    }
    if (shown < world.state().resources.size()) output << "  +" << (world.state().resources.size() - shown);
    return output.str();
}

std::string resolveSceneAsset(const std::string& contentRoot, const std::string& sceneAsset) {
    if (sceneAsset.empty()) return {};
    const std::filesystem::path configured(sceneAsset);
    if (configured.is_absolute()) return configured.lexically_normal().string();
    return (std::filesystem::path(contentRoot) / configured).lexically_normal().string();
}

}  // namespace

int main(int argc, char** argv) {
    std::string profilePath = tide_marks::defaultIntelligenceProfilePath();
    std::string providerOverride = "profile";
    std::string contentRoot = tide_marks::ContentPackLoader::defaultRoot();
    std::string databasePath;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--profile" && index + 1 < argc) profilePath = argv[++index];
        else if (argument == "--provider" && index + 1 < argc) providerOverride = argv[++index];
        else if (argument == "--content-root" && index + 1 < argc) contentRoot = argv[++index];
        else if (argument == "--database" && index + 1 < argc) databasePath = argv[++index];
    }
    std::unique_ptr<tide_marks::IProvider> provider;
    if (providerOverride == "mock") {
        provider = std::make_unique<tide_marks::MockProvider>();
    } else {
        const auto profile = tide_marks::loadIntelligenceProfile(profilePath);
        provider = tide_marks::makeProviderFromProfile(profile, providerOverride);
    }
    tide_marks::WorldService world = [&]() {
        if (databasePath.empty()) return tide_marks::WorldService::load(contentRoot, std::move(provider));
        const bool initialized = std::filesystem::exists(databasePath) &&
                                  std::filesystem::file_size(databasePath) > 0;
        if (initialized) return tide_marks::WorldService::loadSqliteInstance(databasePath, std::move(provider));
        return tide_marks::WorldService::createSqliteInstance(databasePath, contentRoot, std::move(provider));
    }();
    const auto worldName = world.content().instance ? world.content().instance->displayName : world.content().worldId;
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    if (display == nullptr || *display == '\0') {
        std::cerr << "Tide Marks " << worldName
                  << ": the 3D client requires an X11 DISPLAY; use tide_marks_headless or xvfb-run.\n";
        return EXIT_FAILURE;
    }
#endif
    const auto bounds = worldBounds(world);
    InitWindow(1280, 720, (std::string("Tide Marks - ") + worldName).c_str());
    SetTargetFPS(60);
    const auto sceneAssetPath = resolveSceneAsset(contentRoot, world.content().sceneAsset);
    const bool sceneAssetAvailable = !sceneAssetPath.empty() && FileExists(sceneAssetPath.c_str());
    const bool useGlb = sceneAssetAvailable && std::getenv("TIDE_MARKS_USE_GLB") != nullptr;
    Model sceneAsset{};
    if (useGlb) sceneAsset = LoadModel(sceneAssetPath.c_str());
    const float centerX = (bounds.minX + bounds.maxX) * 0.5F;
    const float centerZ = (bounds.minZ + bounds.maxZ) * 0.5F;
    Vector3 player{centerX, 0.65F, centerZ};
    std::string dialogue = "Walk with WASD. Press E near a marker.";
    float timeAccumulator = 0.0F;
    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        const float speed = 4.0F * dt;
        if (IsKeyDown(KEY_W)) player.z -= speed;
        if (IsKeyDown(KEY_S)) player.z += speed;
        if (IsKeyDown(KEY_A)) player.x -= speed;
        if (IsKeyDown(KEY_D)) player.x += speed;
        player.x = clampValue(player.x, bounds.minX, bounds.maxX);
        player.z = clampValue(player.z, bounds.minZ, bounds.maxZ);
        timeAccumulator += dt;
        if (timeAccumulator >= 1.0F) {
            world.advanceMinutes(1);
            timeAccumulator = 0.0F;
        }
        const auto npcs = npcMarkers(world);
        if (IsKeyPressed(KEY_E)) {
            for (const auto& marker : npcs) {
                if (distanceBetween(player, marker.position) < 2.5F) {
                    const auto turn = world.converse(marker.id, "Hello, tell me what is happening.",
                                                      "client-turn-" + std::to_string(world.state().events.size()));
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
        physics->step(0.02);
        const auto physicsEvents = physics->drainEvents();
        if (!physicsEvents.empty()) dialogue = "The world reacts to your movement.";

        BeginDrawing();
        ClearBackground(Color{18, 27, 42, 255});
        Camera3D camera{};
        camera.position = cameraPosition(world.content().cameraMode, bounds);
        camera.target = {centerX, 0.0F, centerZ};
        camera.up = {0.0F, 1.0F, 0.0F};
        camera.fovy = 45.0F;
        camera.projection = CAMERA_PERSPECTIVE;
        BeginMode3D(camera);
        if (useGlb) {
            DrawModel(sceneAsset, {0.0F, 0.0F, 0.0F}, 1.0F, WHITE);
        } else {
            drawWorldGeometry(world);
        }
        DrawCube(player, 0.8F, 1.3F, 0.8F, Color{220, 220, 224, 255});
        for (const auto& marker : npcs) {
            DrawCube(marker.position, 0.75F, 1.5F, 0.75F, stableColor(marker.id));
        }
        EndMode3D();
        DrawRectangle(0, 0, 1280, 84, Color{10, 16, 26, 220});
        DrawText((worldName + "  |  WASD move  E talk  ESC quit").c_str(), 24, 16, 22, RAYWHITE);
        DrawText(dialogue.c_str(), 24, 48, 18, Color{224, 210, 170, 255});
        const auto resourceLine = world.state().now.toString() + "  " + resourceSummary(world);
        DrawText(resourceLine.c_str(), 600, 16, 16, RAYWHITE);
        drawWorldMap(world, player, npcs, bounds);
        EndDrawing();
    }
    if (useGlb) UnloadModel(sceneAsset);
    CloseWindow();
    return 0;
}

#else

// The first client is intentionally dependency-light: it is a deterministic
// scene inspector that can run in CI or over SSH. The raylib/TGUI presentation
// layer will consume the same WorldService/ViewProjection API in the next
// client milestone; no gameplay code depends on a renderer.
int main(int argc, char** argv) {
    std::string contentRoot = tide_marks::ContentPackLoader::defaultRoot();
    std::string databasePath;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--content-root" && index + 1 < argc) contentRoot = argv[++index];
        else if (std::string(argv[index]) == "--database" && index + 1 < argc) databasePath = argv[++index];
    }
    tide_marks::WorldService world = [&]() {
        if (databasePath.empty()) return tide_marks::WorldService::load(contentRoot);
        const bool initialized = std::filesystem::exists(databasePath) &&
                                  std::filesystem::file_size(databasePath) > 0;
        if (initialized) return tide_marks::WorldService::loadSqliteInstance(databasePath);
        return tide_marks::WorldService::createSqliteInstance(databasePath, contentRoot);
    }();
    const auto worldName = world.content().instance ? world.content().instance->displayName : world.content().worldId;
    std::cout << "Tide Marks " << worldName << " client (headless scene preview)\n";
    std::cout << "Renderer disabled. Reconfigure build with -DTIDE_MARKS_FETCH_RAYLIB=ON for the 3D window.\n";
    std::cout << "camera=" << world.content().cameraMode
              << " scene=" << (world.content().sceneAsset.empty() ? "procedural" : world.content().sceneAsset) << "\n";
    std::cout << tide_marks::summarize(world.state()) << '\n';
    for (const auto& [id, npc] : world.state().npcs) {
        std::cout << "npc " << npc.definitionId << " location=" << npc.location << '\n';
    }
    tide_marks::DeterministicPhysicsWorld physics;
    physics.addBody(tide_marks::PhysicsBody{"player", 0.0, 0.0, 0.5, true});
    physics.applyImpulse("player", 1.0, 0.0);
    physics.step(0.02);
    std::cout << "physics_events=" << physics.drainEvents().size() << "\n";
    return 0;
}

#endif
