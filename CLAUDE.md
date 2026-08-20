# OAK (Open Agent Kernel) — CLAUDE.md

## Project Overview

On-device Agent OS framework. C++17, CMake 3.18+.
Open-core model: strategic modules are OSS, kernel runtime is proprietary.

## Build & Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMASTER_AGENT_BUILD_CLI=ON -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

OSS build produces: test binaries + eval harnesses + bench_strategic.
Full CLI (`sparx`) requires proprietary kernel source in `src/`.

## Code Conventions

- C++17, no exceptions in hot paths
- `#pragma once` for all headers
- Namespaces: `master_agent::` (kernel API), `sparx::` (CLI/modules)
- Return errors via `Result<T>`, not exceptions
- Comments explain *why*, not *what*
- Test files: `tests/test_*.cpp`
- Benchmarks: `tests/bench_*.cpp`
- Each module is one .cpp + one .h in `cli/src/` and `cli/include/`

## Key Files

- `include/master_agent/` — Stable public API headers
- `cli/src/sparx_*.cpp` — Strategic feature implementations
- `cli/src/cmd_*.cpp` — CLI commands
- `cli/src/llama_cpp_model_runtime.cpp` — llama-server adapter
- `tests/CMakeLists.txt` — Test target definitions
- `VERSION.json` — Project version metadata
- `ARCHITECTURE.md` — Full code map

## Git Workflow

- Branch from `main`, PR back
- Commit messages: `feat(module):`, `fix(module):`, `docs:`, `chore:`
- Never commit: build artifacts, .gradle/, .cxx/, secrets, Qualcomm-licensed files
- Always run `ctest` before pushing

## Version

0.3.0-alpha. Versions follow semver. Don't inflate beyond actual stability.
