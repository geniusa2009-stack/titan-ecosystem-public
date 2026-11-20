cmake_minimum_required(VERSION 3.16)
project(TITAN_Enterprise_Engine_Lab VERSION 0.5.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(TITAN_CORE ${CMAKE_CURRENT_SOURCE_DIR}/TitanEngine/Plugins/TitanFoundation/Source/TitanOrchestrationCore)
add_library(titan_orchestration STATIC
    ${TITAN_CORE}/Private/TitanWorldScheduler.cpp
    ${TITAN_CORE}/Private/TitanFrameGraph.cpp
    ${TITAN_CORE}/Private/TitanTaskGraph.cpp
    ${TITAN_CORE}/Private/TitanStreamingTelemetry.cpp)
target_include_directories(titan_orchestration PUBLIC ${TITAN_CORE}/Public)
target_compile_options(titan_orchestration PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
find_package(Threads REQUIRED)
target_link_libraries(titan_orchestration PUBLIC Threads::Threads)
enable_testing()
add_executable(titan_tests tests/test_engine.cpp)
target_link_libraries(titan_tests PRIVATE titan_orchestration)
add_test(NAME titan_unit COMMAND titan_tests)
add_executable(titan_benchmark benchmarks/bench_scheduler.cpp)
target_link_libraries(titan_benchmark PRIVATE titan_orchestration)
add_executable(titan_legacy_planner_tests tests/test_legacy_planner.cpp)
target_include_directories(titan_legacy_planner_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/TitanEngine/Plugins/TitanFoundation/Source/TitanSpatialCore/Public)
add_test(NAME titan_legacy_planner COMMAND titan_legacy_planner_tests)

# TITAN original data-oriented ECS, fixed clock and CPU visibility.
set(TITAN_DATA ${CMAKE_CURRENT_SOURCE_DIR}/TitanEngine/Plugins/TitanFoundation/Source/TitanDataCore)
add_library(titan_data STATIC
    ${TITAN_DATA}/Private/TitanECS.cpp
    ${TITAN_DATA}/Private/TitanVisibility.cpp)
target_include_directories(titan_data PUBLIC ${TITAN_DATA}/Public)
target_compile_options(titan_data PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
add_executable(titan_data_tests tests/test_titan_data.cpp)
target_link_libraries(titan_data_tests PRIVATE titan_data)
add_test(NAME titan_data COMMAND titan_data_tests)
add_executable(titan_ecs_benchmark benchmarks/bench_data.cpp)
target_link_libraries(titan_ecs_benchmark PRIVATE titan_data)

add_executable(titan_telemetry_tests tests/test_titan_telemetry.cpp)
target_link_libraries(titan_telemetry_tests PRIVATE titan_orchestration)
add_test(NAME titan_telemetry COMMAND titan_telemetry_tests)
