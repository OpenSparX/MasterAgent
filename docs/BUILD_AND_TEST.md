# Build, Run, and Test

## 1. CMake Targets

- `MasterAgent::Core` — public static library target.
- `master_agent_app` — default application target (output binary: `master_agent`).
- `memory_short_term_core` — bundled short-term memory implementation.

Key options:

```text
MASTER_AGENT_BUILD_APP=ON
MASTER_AGENT_BUILD_TESTS=ON
MASTER_AGENT_ENABLE_IPC=OFF
MASTER_AGENT_ENABLE_QNN=OFF
```

## 2. Native Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMASTER_AGENT_BUILD_APP=ON \
  -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run:

```bash
./build/master_agent \
  --runtime=./runtime \
  "Switch the AC to recirculation mode"
```

Output is JSON containing TurnResult, plan state, and mock model call traces.

## 3. Android arm64

Using the NDK toolchain:

```bash
cmake -S . -B build-android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_static \
  -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build-android-arm64 --parallel
```

`scripts/deploy_android.ps1` only builds and pushes by default. Pass `-RunSmokeTest` explicitly to run one mock flow on the specified device.

## 4. Test Groups

| Target | Focus |
| --- | --- |
| test_e2e | Input, memory, deterministic tasks, two-stage mock, restart |
| test_atomic | MCP, concurrency, priority, preemption, idempotency, Unknown |
| test_atomic_durability | Invocation seal, restart, corruption recovery |
| test_orchestrator_durability | WAL, terminal-state replay, fail-closed |
| test_inference | Replica, KV cache, P0/P2 preemption, cancel, malicious output |
| test_observability | Logging, audit, exceptions, privacy, D4, recovery |
| test_resilience | Deadline, resources, callbacks, and fault injection |
| test_contracts | Cross-module identity, schema, and caller whitelist |
| test_mcp_wire_contracts | MCP JSON-RPC wire protocol |
| test_prompt_skill | Prompt template selection/rendering, skill config load/route/body disclosure |
| test_preprocess | UTF-8, parameters, time, reserved fields, state capability/query and observability deps |
| test_memory_short_term_l1 | L1 raw tests from the memory team (source unmodified) |
| test_ipc | Optional IPC transport (not built by default) |

## 5. Single Module vs Full Suite

Run a single test:

```bash
ctest --test-dir build -R test_inference --output-on-failure
```

Run all:

```bash
ctest --test-dir build --output-on-failure
```

Delivery acceptance includes at minimum: clean configure, full build, automated tests, install-package consumer build, static naming and comment checks, manifest hash verification. A successful Android cross-compilation does not constitute real-vehicle or real-model validation.

## 6. Delivery Verification Record

Delivery consolidation completed 2026-07-31:

- Android NDK r29, arm64-v8a, API 28 default config full build.
- Core library, single-process app, and eleven default test targets compiled; all eleven test groups passed natively.
- `MASTER_AGENT_ENABLE_IPC=ON` config and `test_ipc` compiled.
- Post-install consumer built via `find_package(MasterAgent)` and `MasterAgent::Core`.

This consolidation did not connect a real ADB device, run a real model, or interact with a real vehicle provider per delivery requirements. Cross-compiled test binaries do not equal device-side test passes.
