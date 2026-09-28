# OAK / MasterAgent

An embeddable, local-first C++17 agent runtime: turn input into validated tool
execution, with deterministic skills before model inference.

**0.4.0-alpha — open reference runtime.** Public source now builds a working
`sparx` CLI and an installable C++ SDK. This is a small synchronous runtime, not
the proprietary durable kernel described by the legacy interfaces.

中文：开源版现可独立构建、运行，并作为 C++ SDK 集成。先匹配确定性技能，再按需调用模型；
工具名称和参数必须通过校验。当前会话、历史和请求去重仅保存在内存中，不承诺崩溃恢复。

## Quick start

Requires CMake 3.18+, C++17, and libcurl development files for the optional HTTP
adapter. Tests require Python 3. The release CI targets Linux and macOS.

```bash
git clone https://github.com/OpenSparX/MasterAgent.git
cd MasterAgent
# Ubuntu HTTP dependency: sudo apt-get install libcurl4-openssl-dev
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

For an SDK and deterministic CLI without libcurl:

```bash
cmake -S . -B build-offline -DMASTER_AGENT_ENABLE_HTTP=OFF -DBUILD_EVAL=OFF
cmake --build build-offline --parallel 4
```

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

## Capability status

| Capability | Public reference release |
|---|---|
| Deterministic exact-phrase skills | Available, no model required |
| Host-defined tools and argument validation | Available; primitive object schema subset |
| Local model HTTP adapter | Available when built with libcurl |
| Sessions and request deduplication | Available in memory; bounded; no restart persistence |
| CLI and relocatable CMake SDK package | Built and exercised by CI |
| Speculation, mesh, verification, learning, decoding | Experimental modules and synthetic evaluations; not wired into the reference CLI |
| Edge/cloud harness | Experimental; explicit opt-in; bounded outstanding inference workers |
| Durable DAG/WAL recovery and legacy kernel factories | Not supplied by this reference runtime |
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
archive. The package uses platform system dependencies, including libcurl when
enabled; it is not a universally static binary.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Changes must keep Release assertions
active and pass the CLI, HTTP protocol fixture, installed consumer, and runtime
failure tests. Hardware and actual model validation should record the model,
quantization, device, workload, and latency distribution separately.

Apache-2.0. See [LICENSE](LICENSE).
