# Local verification — 2026-11-05

- Existing portable C++14 core: configured and compiled using CMake Release.
- CTest: `titan_unit` and `titan_legacy_planner` PASS.
- Python installer: 5 tests PASS (dry-run, install, overwrite refusal, version mismatch, bad tree).
- UE4.27 API compatibility: manually checked `GameplayStatics.h` and `LevelStreaming.h` signatures in `AlexMercer-MA/UnrealEngine-4.27` repository.
- Actual Unreal Engine compile, UE Editor runtime, GPU frame time and real streaming levels: NOT TESTED here.
