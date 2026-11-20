cmake_minimum_required(VERSION 3.24)
project(TITAN VERSION 0.6.0 LANGUAGES CXX)

option(TITAN_BUILD_STUDIO "Build TITAN Studio (requires Qt >= 6.7)" ON)
option(TITAN_BUILD_TESTS "Build automated tests" ON)
option(TITAN_BUILD_LEGACY "Build and test the imported TITAN v0.5 portable core" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(TITAN_WARNINGS $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wno-unused-parameter>)
set(TITAN_RUNTIME_DIR ${CMAKE_BINARY_DIR}/bin)
set(TITAN_EXTENSIONS_DIR ${CMAKE_BINARY_DIR}/extensions)

# Writable UE 5.8.4 fork (decision D-1) and the TITAN host project synced next to it.
set(TITAN_UE_WORKING_COPY "${CMAKE_CURRENT_SOURCE_DIR}/../engine/UE-5.8.4" CACHE PATH "Writable UE 5.8.4 fork")
get_filename_component(TITAN_UE_WORKING_COPY_REAL "${TITAN_UE_WORKING_COPY}" REALPATH)
get_filename_component(TITAN_UE_PARENT "${TITAN_UE_WORKING_COPY_REAL}" DIRECTORY)
set(TITAN_UE_EDITOR_EXE "${TITAN_UE_WORKING_COPY_REAL}/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor")
set(TITAN_UE_HOST_PROJECT "${TITAN_UE_PARENT}/TitanHost/TitanHost.uproject")
set(TITAN_BRIDGE_PUBLIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/unreal/Plugins/TitanStudioBridge/Source/TitanStudioBridge/Public")

# ---- titan_core: engine/editor shared contracts (no third-party dependencies) ----------
add_library(titan_core STATIC
  core/src/json.cpp
  core/src/json_number.cpp
  core/src/math.cpp
  core/src/scene.cpp
  core/src/edit.cpp
  core/src/project.cpp
  core/src/protocol.cpp
  core/src/mesh.cpp
  core/src/sha256.cpp
  core/src/assets.cpp
  core/src/asset_importers.cpp
  core/src/prefab.cpp
  core/src/clipboard.cpp
  core/src/components.cpp
  core/src/play_session.cpp
  core/src/script_graph.cpp
  core/src/timeline.cpp
  core/src/input_map.cpp
  core/src/material.cpp
  core/src/event_bus.cpp
  core/src/physics_geometry.cpp
  core/src/physics.cpp
  core/src/audio.cpp
  core/src/spatial.cpp
  core/src/navigation.cpp
  core/src/blackboard.cpp
  core/src/behavior_tree.cpp
  core/src/utility_ai.cpp
  core/src/perception.cpp
  core/src/scene_template.cpp
  core/src/capabilities.cpp
  $<$<NOT:$<PLATFORM_ID:Windows>>:${CMAKE_CURRENT_SOURCE_DIR}/core/src/ipc_posix.cpp>)
target_include_directories(titan_core PUBLIC core/include sdk/include)
target_compile_options(titan_core PRIVATE ${TITAN_WARNINGS})
set_target_properties(titan_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- titan_engine: authoritative session + extension host ----------------------------
add_library(titan_engine STATIC engine/host/engine_session.cpp engine/host/extension_host.cpp)
target_include_directories(titan_engine PUBLIC engine/host)
target_link_libraries(titan_engine PUBLIC titan_core ${CMAKE_DL_LIBS})
target_compile_options(titan_engine PRIVATE ${TITAN_WARNINGS})

add_executable(titan-engine-host engine/host/main.cpp)
target_link_libraries(titan-engine-host PRIVATE titan_engine)
target_compile_options(titan-engine-host PRIVATE ${TITAN_WARNINGS})
set_target_properties(titan-engine-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TITAN_RUNTIME_DIR})

# ---- TITAN extensions ----------------------------------------------------------------
function(titan_add_extension target ext_id src_dir)
  add_library(${target} MODULE ${ARGN})
  target_link_libraries(${target} PRIVATE titan_core)
  target_compile_options(${target} PRIVATE ${TITAN_WARNINGS})
  set_target_properties(${target} PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${TITAN_EXTENSIONS_DIR}/${ext_id})
  set(LIBRARY_NAME "${target}${CMAKE_SHARED_MODULE_SUFFIX}")
  configure_file(${src_dir}/titan-extension.json ${TITAN_EXTENSIONS_DIR}/${ext_id}/titan-extension.json @ONLY)
endfunction()

titan_add_extension(titan_ext_procedural_scatter titan.procedural.scatter
  ${CMAKE_CURRENT_SOURCE_DIR}/extensions/procedural-scatter
  extensions/procedural-scatter/scatter.cpp)

# ---- tools ----------------------------------------------------------------------------
add_executable(titan-protocol-check tools/protocol-check/main.cpp)
target_link_libraries(titan-protocol-check PRIVATE titan_core)
target_include_directories(titan-protocol-check PRIVATE ${TITAN_BRIDGE_PUBLIC_DIR})

add_executable(titan-asset-bench tools/asset-bench/main.cpp)
target_link_libraries(titan-asset-bench PRIVATE titan_core)
target_compile_options(titan-asset-bench PRIVATE ${TITAN_WARNINGS})
set_target_properties(titan-asset-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TITAN_RUNTIME_DIR})
add_executable(titan-ue-bench tools/ue-bench/main.cpp)
target_link_libraries(titan-ue-bench PRIVATE titan_core)
target_compile_options(titan-ue-bench PRIVATE ${TITAN_WARNINGS})
set_target_properties(titan-ue-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TITAN_RUNTIME_DIR})
set_target_properties(titan-protocol-check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TITAN_RUNTIME_DIR})

# ---- tests ----------------------------------------------------------------------------
if(TITAN_BUILD_TESTS)
  enable_testing()
  add_executable(titan_core_tests tests/test_core.cpp)
  target_link_libraries(titan_core_tests PRIVATE titan_core)
  add_test(NAME core.unit COMMAND titan_core_tests)
  add_executable(titan_assets_tests tests/test_assets.cpp)
  target_link_libraries(titan_assets_tests PRIVATE titan_core)
  target_compile_options(titan_assets_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.assets COMMAND titan_assets_tests)
  add_executable(titan_prefab_tests tests/test_prefab.cpp)
  target_link_libraries(titan_prefab_tests PRIVATE titan_core)
  target_compile_options(titan_prefab_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.prefab COMMAND titan_prefab_tests)
  add_executable(titan_script_tests tests/test_script.cpp)
  target_link_libraries(titan_script_tests PRIVATE titan_core)
  target_compile_options(titan_script_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.script COMMAND titan_script_tests)
  add_executable(titan_timeline_tests tests/test_timeline.cpp)
  target_link_libraries(titan_timeline_tests PRIVATE titan_core)
  target_compile_options(titan_timeline_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.timeline COMMAND titan_timeline_tests)
  add_executable(titan_input_tests tests/test_input.cpp)
  target_link_libraries(titan_input_tests PRIVATE titan_core)
  target_compile_options(titan_input_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.input COMMAND titan_input_tests)
  add_executable(titan_material_tests tests/test_material.cpp)
  target_link_libraries(titan_material_tests PRIVATE titan_core)
  target_compile_options(titan_material_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.material COMMAND titan_material_tests)
  add_executable(titan_event_bus_tests tests/test_event_bus.cpp)
  target_link_libraries(titan_event_bus_tests PRIVATE titan_core)
  target_compile_options(titan_event_bus_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.event-bus COMMAND titan_event_bus_tests)
  add_executable(titan_physics_tests tests/test_physics.cpp)
  target_link_libraries(titan_physics_tests PRIVATE titan_core)
  target_compile_options(titan_physics_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.physics COMMAND titan_physics_tests)
  add_executable(titan_audio_tests tests/test_audio.cpp)
  target_link_libraries(titan_audio_tests PRIVATE titan_core)
  target_compile_options(titan_audio_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.audio COMMAND titan_audio_tests)
  add_executable(titan_spatial_tests tests/test_spatial.cpp)
  target_link_libraries(titan_spatial_tests PRIVATE titan_core)
  target_compile_options(titan_spatial_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.spatial COMMAND titan_spatial_tests)
  add_executable(titan_navigation_tests tests/test_navigation.cpp)
  target_link_libraries(titan_navigation_tests PRIVATE titan_core)
  target_compile_options(titan_navigation_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.navigation COMMAND titan_navigation_tests)
  add_executable(titan_behavior_tree_tests tests/test_behavior_tree.cpp)
  target_link_libraries(titan_behavior_tree_tests PRIVATE titan_core)
  target_compile_options(titan_behavior_tree_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.behavior-tree COMMAND titan_behavior_tree_tests)
  add_executable(titan_utility_ai_tests tests/test_utility_ai.cpp)
  target_link_libraries(titan_utility_ai_tests PRIVATE titan_core)
  target_compile_options(titan_utility_ai_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.utility-ai COMMAND titan_utility_ai_tests)
  add_executable(titan_perception_tests tests/test_perception.cpp)
  target_link_libraries(titan_perception_tests PRIVATE titan_core)
  target_compile_options(titan_perception_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME core.perception COMMAND titan_perception_tests)

  add_executable(titan_extension_tests tests/test_extensions.cpp)
  target_link_libraries(titan_extension_tests PRIVATE titan_engine)
  target_compile_definitions(titan_extension_tests PRIVATE TITAN_EXTENSIONS_DIR="${TITAN_EXTENSIONS_DIR}")
  add_dependencies(titan_extension_tests titan_ext_procedural_scatter)
  add_test(NAME extensions.unit COMMAND titan_extension_tests)

  add_executable(titan_host_integration_tests tests/test_host_integration.cpp)
  target_link_libraries(titan_host_integration_tests PRIVATE titan_core)
  target_compile_definitions(titan_host_integration_tests PRIVATE
    TITAN_HOST_EXE="$<TARGET_FILE:titan-engine-host>" TITAN_EXTENSIONS_DIR="${TITAN_EXTENSIONS_DIR}")
  add_dependencies(titan_host_integration_tests titan-engine-host titan_ext_procedural_scatter)
  add_test(NAME host.integration COMMAND titan_host_integration_tests)

  # TITAN <-> Unreal conversions and frame layout, compiled from the bridge plugin's own headers.
  add_executable(titan_ue_coordinates_tests tests/test_ue_coordinates.cpp)
  target_link_libraries(titan_ue_coordinates_tests PRIVATE titan_core)
  target_include_directories(titan_ue_coordinates_tests PRIVATE ${TITAN_BRIDGE_PUBLIC_DIR})
  target_compile_options(titan_ue_coordinates_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME ue.coordinates COMMAND titan_ue_coordinates_tests)

  add_test(NAME host.protocol-conformance
    COMMAND ${CMAKE_COMMAND} -DHOST=$<TARGET_FILE:titan-engine-host> -DCHECK=$<TARGET_FILE:titan-protocol-check>
            -DWORK=${CMAKE_BINARY_DIR}/conformance -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_conformance.cmake)

  # UE 5.8.4 source contract (decision D-1). Static check; runs only if the engine tree exists.
  set(TITAN_UE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vault/extracted/ue-5.8.4/UnrealEngine-release"
      CACHE PATH "Unreal Engine 5.8.4 source root used by TITAN")
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND EXISTS "${TITAN_UE_ROOT}/Engine/Build/Build.version")
    add_test(NAME ue.source-contract
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_ue_source_contract.py
              --engine ${TITAN_UE_ROOT} --titan ${CMAKE_CURRENT_SOURCE_DIR} --out ${CMAKE_BINARY_DIR}/ue-contract.json)
  endif()
  # Per-stage, per-volume disk preflight and the build script's disk guard (fake engine, simulated low space).
  if(Python3_FOUND)
    add_test(NAME tools.preflight
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ue_build_preflight.py ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(tools.preflight PROPERTIES TIMEOUT 300)
  endif()
  # Real Unreal Engine 5.8.4 + TitanStudioBridge (label "unreal"). Skipped (not passed) until the editor is built.
  add_test(NAME ue.bridge-conformance
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ue_bridge.sh conformance ${TITAN_UE_EDITOR_EXE}
            ${TITAN_UE_HOST_PROJECT} $<TARGET_FILE:titan-protocol-check> ${CMAKE_BINARY_DIR}/ue-conformance)
  add_test(NAME ue.automation
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ue_bridge.sh automation ${TITAN_UE_EDITOR_EXE}
            ${TITAN_UE_HOST_PROJECT} ${CMAKE_BINARY_DIR}/ue-automation)
  set_tests_properties(ue.bridge-conformance ue.automation PROPERTIES
    LABELS unreal SKIP_RETURN_CODE 77 TIMEOUT 5400 RUN_SERIAL TRUE)
  # The engine working copy (branch titan/engine) must carry the same plugin sources as this repo.
  if(EXISTS "${TITAN_UE_WORKING_COPY}/Engine/Build/Build.version")
    add_test(NAME ue.plugin-sync
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/sync_ue_plugins.sh --check ${TITAN_UE_WORKING_COPY})
  endif()
endif()

if(TITAN_BUILD_LEGACY)
  add_subdirectory(legacy/TitanFoundation-v0.5 legacy-v0.5)
endif()

if(TITAN_BUILD_STUDIO)
  add_subdirectory(studio)
endif()
