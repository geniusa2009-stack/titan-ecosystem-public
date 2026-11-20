# TITAN v0.4 verified scope

- Standalone C++14 core: built with GCC and Clang (Linux).
- CTest: 3 registered test programs, including the new `titan_data_tests` with 8 named test suites.
- Clang ASan + UBSan: same tests executed with runtime sanitizers; no reported violations in tested cases.
- Python installer: 5 unit tests for dry-run, safe copying, refusal to overwrite, version checking, and missing source tree.
- Benchmark: creates 100,000 entities and runs 100 logical simulation updates, then computes CPU visibility/batches for 100,000 synthetic render objects; results are environment-specific.
- **Not tested:** Unreal Build Tool compilation, Xcode, macOS M3, actual graphics hardware drawing, production open worlds, ECS thread safety, third-party commercial licensing.

Validation commands:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_install_*.py' -v
```

The test suite checks algorithmic invariants, not all 3D rendering functionality. Running on the user's source checkout should start with a **separate Git branch**, successful Unreal project generation and a full UE4.27 compile; no claims of success until those logs exist.
