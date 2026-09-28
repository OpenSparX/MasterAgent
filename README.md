# OAK / MasterAgent

An embeddable, local-first C++17 agent runtime: turn input into validated tool
execution, with deterministic skills before model inference.

**0.4.0-alpha.3 — open reference runtime with contextual tools and durable receipts.** Public source now builds a working
`sparx` CLI and an installable C++ SDK. This is a small synchronous runtime, not
the proprietary durable kernel described by the legacy interfaces.

中文：开源版现可独立构建、运行，并作为 C++ SDK 集成。先匹配确定性技能，再按需调用模型；
工具名称和参数必须通过校验。默认使用内存；显式启用 SQLite 后，支持跨重启请求去重、历史恢复、UNKNOWN 故障恢复和人工对账。

## Quick start

Requires CMake 3.18+, C++17, and libcurl development files for the optional HTTP
adapter, plus SQLite development files for durable storage. Tests/packaging require Python 3. The release CI targets Linux and macOS.

```bash
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
# Ubuntu HTTP dependency: sudo apt-get install libcurl4-openssl-dev libsqlite3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/cli/sparx demo automotive
```

The demo sets an **in-memory simulated** AC temperature, then reads it back.
It needs no model, credentials, network, or vehicle hardware. Each output is JSON.

```bash
./build/cli/sparx run --input '把空调调到22度'
./build/cli/sparx run --input 'vehicle status'
```

Each process starts a new demo state. To preserve state between requests in a
single process, run `sparx run` and enter one input per line until EOF.
An unmatched phrase returns `NO_ROUTE` unless a model is explicitly configured.

## Use an existing local model server

Start a compatible llama-server separately with your chosen GGUF model, then:

```bash
./build/cli/sparx run --endpoint 127.0.0.1:8080 --model local \
  --input 'Set the AC to 24 degrees'
```

The adapter sends non-streaming `/v1/chat/completions` requests. The runtime asks
for a JSON response or a single tool call, rejects unknown tools and invalid
arguments, and returns the actual tool result. Model compliance depends on the
model; malformed output produces an error, never a guessed tool call. No model
is downloaded or started by this CLI. A full HTTP(S) endpoint URL may also be
provided explicitly; no cloud fallback happens automatically.

A request timeout can be set with `--timeout-ms 3000`. To use an authenticated
endpoint, pass the environment-variable **name** via `--api-key-env MODEL_API_KEY`;
credentials are not supplied as CLI arguments.

For an SDK and deterministic CLI without libcurl:

```bash
cmake -S . -B build-offline -DMASTER_AGENT_ENABLE_HTTP=OFF -DBUILD_EVAL=OFF
cmake --build build-offline --parallel 4
```

## Persist requests and recover interrupted tools

```bash
./build/cli/sparx run --state ./state/agent.db --session alice --request-id ac-001 \
  --input 'set AC to 22 degrees'
./build/cli/sparx recover --state ./state/agent.db
```

Repeating the same request after restarting returns the recorded outcome without
calling the tool again. Unfinished requests become `UNKNOWN`, require external
verification, and can be explicitly reconciled. A database failure blocks further
execution instead of silently switching to memory. See the [recovery guide](docs/durable_recovery.md)
for JSONL input, ownership locks, audit notes, backups and failure behavior.

Only execution receipts/history are persisted; the AC simulator itself still resets
on process restart. There is no transaction spanning a real device and SQLite, so this
is not a distributed exactly-once guarantee. Storage is not encrypted.

To omit both optional dependencies, build with `MASTER_AGENT_ENABLE_HTTP=OFF` and
`MASTER_AGENT_ENABLE_STORAGE=OFF`.

## Embed the SDK

```bash
cmake --install build --prefix "$PWD/install-sdk"
cmake -S examples/reference_agent -B build-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/install-sdk"
cmake --build build-consumer
ctest --test-dir build-consumer --output-on-failure
```

Consumer CMake:

```cmake
find_package(MasterAgent CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE MasterAgent::Core)
# Optional HTTP adapter:
# target_link_libraries(your_app PRIVATE MasterAgent::Http)
```

The public reference API is in
[`reference_runtime.h`](include/master_agent/runtime/reference_runtime.h).
Register a tool with a supported parameter schema and callback, register exact
phrase skills, optionally set a model callback, then call `Runtime::run()`.
See the [integration contract](docs/reference_runtime.md) for errors, schema
support, sessions, request replay, and concurrency limitations.

## Connect a business tool

Context-aware tools receive the session/request identity, a stable scoped
idempotency key, deadline and cooperative cancellation signal. They return an
explicit committed, failed or unknown outcome. The existing `registerTool()` API
remains available. See the [execution contract](docs/reference_runtime.md).

The [inventory example](examples/inventory_agent/README.md) talks to a separate
HTTP service that commits inventory and an idempotency receipt in its own SQLite
database. Its integration test deliberately drops a response after commit, checks
UNKNOWN recovery, reconciles against the service receipt and proves no second
stock decrement after either process restarts. This is a local business-service
integration example, not a real-model or third-party production validation.

## Capability status

| Capability | Public reference release |
|---|---|
| Deterministic exact-phrase skills | Available, no model required |
| Host-defined tools and argument validation | Available; primitive object schema subset |
| Execution context and typed tool outcomes | Available; identity, deadline, cooperative cancellation, committed/failed/unknown |
| Independent HTTP business example | Inventory service with atomic idempotency and lost-response recovery |
| Local model HTTP adapter | Available when built with libcurl |
| Sessions and request deduplication | In memory, or persistent SQLite receipts/history with explicit opt-in |
| Crash recovery and reconciliation | Interrupted single requests become UNKNOWN; explicit evidence-based reconciliation |
| CLI and relocatable CMake SDK package | Built and exercised by CI |
| Speculation, mesh, verification, learning, decoding | Experimental modules and synthetic evaluations; not wired into the reference CLI |
| Edge/cloud harness | Experimental; explicit opt-in; bounded outstanding inference workers |
| Multi-step durable DAG and legacy kernel factories | Not supplied by this reference runtime |
| Qualcomm QNN/Genie integration | Platform integration required; not verified by this release |
| Production encryption / DP learning guarantees | Not claimed for this release |

Other headers under `include/master_agent/` describe legacy kernel contracts;
including them does not mean their factories have public implementations. Use
`master_agent::reference` for the supported open runtime entry point.

## Tests, evaluations, and releases

```bash
ctest --test-dir build --output-on-failure
bash eval/run_all.sh
bash scripts/package.sh build dist/sparx-sdk.tar.gz
```

Evaluations use synthetic workloads (learning includes a simulated model), not
end-to-end evidence for real-model latency, personalization, or NPU performance.
Results are written to `build/eval-results/`. Missing or failed evaluations
return a nonzero status. `BUILD_EVAL=OFF` excludes them from an SDK-only build.

The package script installs the SDK and CLI, unpacks the archive in a new
location, runs the demo, and builds an external consumer before publishing the
archive. The archive also includes the inventory example; HTTP/storage-enabled packages build and exercise it after unpacking. The archive includes `BUILD_INFO.json` and a SHA-256 sidecar. The package uses
platform system dependencies, including libcurl and SQLite when enabled; it is not a universally static binary.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Changes must keep Release assertions
active and pass the CLI, HTTP protocol fixture, installed consumer, and runtime
failure tests. Hardware and actual model validation should record the model,
quantization, device, workload, and latency distribution separately.

Apache-2.0. See [LICENSE](LICENSE).
