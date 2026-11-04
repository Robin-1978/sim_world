cmake_minimum_required(VERSION 3.20)

project(tide_marks LANGUAGES CXX)

find_package(Threads REQUIRED)

option(TIDE_MARKS_ENABLE_SQLITE "Build the optional SQLite persistence adapter when sqlite3 is available" ON)
option(TIDE_MARKS_REQUIRE_SQLITE "Fail configuration when the SQLite persistence adapter cannot be built" OFF)
if(TIDE_MARKS_ENABLE_SQLITE)
    find_path(TIDE_MARKS_SQLITE_INCLUDE_DIR sqlite3.h)
    find_library(TIDE_MARKS_SQLITE_LIBRARY NAMES sqlite3)
endif()
if(TIDE_MARKS_REQUIRE_SQLITE AND (NOT TIDE_MARKS_ENABLE_SQLITE OR
                                  NOT TIDE_MARKS_SQLITE_INCLUDE_DIR OR
                                  NOT TIDE_MARKS_SQLITE_LIBRARY))
    message(FATAL_ERROR "TIDE_MARKS_REQUIRE_SQLITE=ON but sqlite3 headers or library were not found")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TIDE_MARKS_WARNINGS "Enable compiler warnings" ON)
option(TIDE_MARKS_FETCH_ENGINE_DEPS "Fetch raylib and Bullet for the renderer/physics adapter" OFF)
option(TIDE_MARKS_FETCH_RAYLIB "Fetch raylib for the interactive client" OFF)
option(TIDE_MARKS_FETCH_BULLET "Fetch Bullet 3.25 for the production physics adapter" OFF)

if(TIDE_MARKS_FETCH_ENGINE_DEPS)
    set(TIDE_MARKS_FETCH_RAYLIB ON)
    set(TIDE_MARKS_FETCH_BULLET ON)
endif()

include(FetchContent)

# Dependencies are pinned and owned by this build. A machine's globally
# installed headers are never selected by the project.
set(FETCHCONTENT_QUIET OFF)
# Keep the default build reproducible offline. The pinned nlohmann/json source
# is checked into third_party and is still consumed through FetchContent; a
# downstream packager may point this variable at an audited source mirror.
set(TIDE_MARKS_NLOHMANN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json" CACHE PATH
    "Source directory for the pinned nlohmann/json dependency")
if(EXISTS "${TIDE_MARKS_NLOHMANN_SOURCE_DIR}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON "${TIDE_MARKS_NLOHMANN_SOURCE_DIR}")
endif()
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

if(TIDE_MARKS_FETCH_RAYLIB)
    FetchContent_Declare(
        raylib
        URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    )
    FetchContent_MakeAvailable(raylib)
endif()

if(TIDE_MARKS_FETCH_BULLET)
    set(BUILD_BULLET3_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        bullet3
        URL https://github.com/bulletphysics/bullet3/archive/refs/tags/3.25.tar.gz
    )
    FetchContent_MakeAvailable(bullet3)
endif()

add_library(tide_marks_core
    src/core/event_bus.cpp
    src/core/entity_registry.cpp
    src/core/types.cpp
    src/content/content_pack.cpp
    src/evolution/anomaly.cpp
    src/evolution/evolution.cpp
    src/evolution/scenario.cpp
    src/npc/action_validator.cpp
    src/npc/behavior_tree.cpp
    src/npc/behavior_recipe.cpp
    src/npc/conversation.cpp
    src/npc/drive_system.cpp
    src/npc/goal_selector.cpp
    src/npc/state_machine.cpp
    src/navigation/path_planner.cpp
    src/physics/physics_world.cpp
    src/provider/provider.cpp
    src/provider/provider_profile.cpp
    src/provider/decision_context_projector.cpp
    src/provider/intelligence_coordinator.cpp
    src/provider/turn_queue.cpp
    src/provider/evolution_queue.cpp
    src/world/meta_actions.cpp
    src/world/meta_action_executor.cpp
    src/world/resource_system.cpp
    src/world/schedule_system.cpp
    src/world/social_system.cpp
    src/world/event_journal.cpp
        src/world/persistence.cpp
    src/world/simulation_scheduler.cpp
    src/world/simulation_coordinator.cpp
    src/world/transaction_manager.cpp
    src/world/contract_system.cpp
    src/world/npc_simulation_system.cpp
    src/world/behavior_execution_system.cpp
    src/world/command_dispatcher.cpp
    src/world/world_intelligence_system.cpp
    src/world/world_evolution_system.cpp
    src/world/security_system.cpp
    src/world/cognition_system.cpp
    src/world/stimulus_system.cpp
    src/world/entity_projection_system.cpp
    src/world/world_service.cpp
    src/world/world_service_persistence.cpp
    src/world/world_state_json.cpp
)
target_include_directories(tide_marks_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(tide_marks_core PRIVATE
    TIDE_MARKS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(tide_marks_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(TIDE_MARKS_ENABLE_SQLITE AND TIDE_MARKS_SQLITE_INCLUDE_DIR AND TIDE_MARKS_SQLITE_LIBRARY)
    target_compile_definitions(tide_marks_core PUBLIC TIDE_MARKS_HAS_SQLITE=1)
    target_include_directories(tide_marks_core PUBLIC ${TIDE_MARKS_SQLITE_INCLUDE_DIR})
    target_link_libraries(tide_marks_core PUBLIC ${TIDE_MARKS_SQLITE_LIBRARY})
endif()
if(TIDE_MARKS_WARNINGS)
    if(MSVC)
        target_compile_options(tide_marks_core PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tide_marks_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endif()

add_executable(tide_marks_headless src/headless_main.cpp)
target_link_libraries(tide_marks_headless PRIVATE tide_marks_core)

add_executable(tide_marks_client src/client_main.cpp)
target_link_libraries(tide_marks_client PRIVATE tide_marks_core)

add_executable(tide_marks_text src/text_ui_main.cpp)
target_link_libraries(tide_marks_text PRIVATE tide_marks_core)
if(TIDE_MARKS_FETCH_RAYLIB)
    target_compile_definitions(tide_marks_client PRIVATE TIDE_MARKS_HAS_ENGINE_DEPS=1 TIDE_MARKS_HAS_RAYLIB=1)
    target_link_libraries(tide_marks_client PRIVATE raylib)
endif()
if(TIDE_MARKS_FETCH_BULLET)
    target_sources(tide_marks_core PRIVATE src/physics/bullet_physics.cpp)
    target_compile_definitions(tide_marks_core PRIVATE TIDE_MARKS_HAS_BULLET=1)
    target_include_directories(tide_marks_core PRIVATE ${bullet3_SOURCE_DIR}/src)
    target_link_libraries(tide_marks_core PRIVATE BulletDynamics BulletCollision LinearMath)
    target_compile_definitions(tide_marks_client PRIVATE TIDE_MARKS_HAS_BULLET=1)
    target_link_libraries(tide_marks_client PRIVATE BulletDynamics BulletCollision LinearMath)
endif()

enable_testing()
add_executable(tide_marks_tests tests/tide_marks_tests.cpp)
target_link_libraries(tide_marks_tests PRIVATE tide_marks_core)
target_compile_definitions(tide_marks_tests PRIVATE
    TIDE_MARKS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
if(TIDE_MARKS_FETCH_BULLET)
    target_compile_definitions(tide_marks_tests PRIVATE TIDE_MARKS_HAS_BULLET=1)
    target_include_directories(tide_marks_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
add_test(NAME tide_marks_tests COMMAND tide_marks_tests)
add_test(NAME glass_orchard_headless
    COMMAND tide_marks_headless --content-root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/glass-orchard-pack
            --days 0 --provider mock)
set_tests_properties(glass_orchard_headless PROPERTIES
    PASS_REGULAR_EXPRESSION "The Glass Orchard"
    FAIL_REGULAR_EXPRESSION "unknown_npc")
add_test(NAME glass_orchard_replay
    COMMAND tide_marks_headless --content-root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/glass-orchard-pack
            --days 0 --provider mock --replay ${CMAKE_CURRENT_BINARY_DIR}/glass-orchard-replay.jsonl)
set_tests_properties(glass_orchard_replay PROPERTIES
    PASS_REGULAR_EXPRESSION "replay: glass_orchard_world"
    FAIL_REGULAR_EXPRESSION "replay error")
add_test(NAME glass_orchard_text
    COMMAND tide_marks_text --content-root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/glass-orchard-pack
            --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/glass-orchard-pack/session.txt --provider mock)
set_tests_properties(glass_orchard_text PROPERTIES
    PASS_REGULAR_EXPRESSION "talk to a loaded NPC: orin, kel"
    FAIL_REGULAR_EXPRESSION "unknown_npc")
add_test(NAME port_exchange_contract
    COMMAND tide_marks_text --content-root ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/port-pack
            --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/port-pack/exchange-session.txt --provider mock)
set_tests_properties(port_exchange_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "exchange-1 status=proposed|exchange.settled rhea -> exchange-1"
    FAIL_REGULAR_EXPRESSION "invalid_exchange|insufficient_resource|unknown_command")

set_target_properties(tide_marks_headless tide_marks_client tide_marks_text tide_marks_tests
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
