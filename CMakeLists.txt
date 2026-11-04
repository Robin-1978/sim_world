cmake_minimum_required(VERSION 3.20)

project(tide_marks LANGUAGES CXX)

find_package(Threads REQUIRED)

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
    src/evolution/evolution.cpp
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
    src/provider/turn_queue.cpp
    src/world/meta_actions.cpp
    src/world/world_service.cpp
    src/world/world_state_json.cpp
)
target_include_directories(tide_marks_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(tide_marks_core PRIVATE
    TIDE_MARKS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(tide_marks_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
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

set_target_properties(tide_marks_headless tide_marks_client tide_marks_text tide_marks_tests
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
