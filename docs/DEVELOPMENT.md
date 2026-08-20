# Development Guide

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
      -DMASTER_AGENT_BUILD_CLI=ON \
      -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```

## Running Tests

```bash
ctest --test-dir build --output-on-failure
```

## Project Structure

```
├── include/master_agent/    # Public C++ headers (stable API surface)
│   ├── orchestrator/        # DAG task orchestration + WAL recovery
│   └── inference/           # Model runtime interface
├── cli/
│   ├── include/             # Internal headers for strategic features
│   └── src/                 # Implementation
│       ├── sparx_agent_scheduler.cpp    # OS-level agent process management
│       ├── sparx_speculative.cpp        # LSTM intent prediction + HNSW cache
│       ├── sparx_formal_verify.cpp      # CDCL SAT solver for plan verification
│       ├── sparx_mesh.cpp               # mDNS + CRDT + Merkle anti-entropy
│       ├── sparx_learning.cpp           # On-device DP-SGD fine-tuning
│       ├── sparx_constrained_decode.cpp # GBNF grammar-enforced generation
│       ├── llama_cpp_model_runtime.cpp  # llama-server HTTP adapter
│       └── genie_model_runtime.cpp      # Qualcomm QNN/GenieX adapter
├── tests/                   # Unit + integration tests
├── eval/                    # Benchmark evaluation harnesses
├── android/                 # Android demo APK (Kotlin)
├── examples/                # Example agent configurations
└── docs/                    # Architecture docs + technical reports
```

## Adding a New Feature

1. Write the interface in `include/master_agent/` (if it's a kernel API)
   or `cli/include/` (if it's a strategic module)
2. Implement in `cli/src/`
3. Add a test in `tests/`
4. Update `tests/CMakeLists.txt` using `add_master_agent_test()`
5. Run the full test suite before submitting a PR

## Code Style

- C++17, no exceptions in hot paths
- Headers use `#pragma once`
- Namespaces: `master_agent::` for kernel, `sparx::` for CLI features
- Use `std::expected` patterns (returning error codes, not throwing)
- Comments: explain *why*, not *what*

## Running with a Real Model

```bash
# Download a small model for testing
wget https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf

# Start llama-server
llama-server -m qwen2.5-0.5b-instruct-q4_k_m.gguf --port 8080

# Run sparx CLI connected to it
./build/cli/sparx run --endpoint 127.0.0.1:8080
```
