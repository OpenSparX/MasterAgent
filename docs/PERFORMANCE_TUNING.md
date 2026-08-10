# Performance Tuning Guide

Sparx is designed for ultra-low latency (<100ms) on edge devices. This guide covers optimization strategies for production deployments.

---

## 1. NPU Configuration

### QNN Backend Selection

Sparx auto-detects the best backend, but you can override:

```yaml
# agent.yaml
model:
  backend: qnn_htp  # Force Qualcomm HTP (Hexagon Tensor Processor)
  # Options: qnn_htp, qnn_dsp, qnn_gpu, cpu, auto
```

**Recommendations by Platform:**
- **SA8650 (Automotive)**: `qnn_htp` — dedicated NPU, best performance
- **SA8155/8295**: `qnn_dsp` — older DSP-based path, still sub-100ms
- **Development machines**: `cpu` or `qnn_gpu` (CUDA/OpenCL fallback)

### Context Length vs. Latency

Shorter context = faster inference. Tune based on your use case:

| Context Length | Typical Latency | Use Case |
|----------------|-----------------|----------|
| 512 tokens     | 30-50ms         | Command-and-control (automotive, IoT) |
| 2048 tokens    | 80-120ms        | Conversational agents |
| 8192 tokens    | 300-500ms       | Document analysis |

```yaml
model:
  context_length: 512        # ← Start small, grow only if needed
  max_output_tokens: 64      # Limit response length
```

---

## 2. Memory Management

### Short-Term Memory Size

Every memory entry is re-encoded on each turn. Keep it lean:

```yaml
memory:
  short_term:
    max_entries: 5           # Last 5 interactions only
    retention_turns: 3       # Drop after 3 turns
```

**Symptom of oversized memory:** Latency creeps up from 80ms → 200ms over a session.

### WAL Checkpoint Frequency

Write-Ahead Logging ensures crash recovery but adds I/O overhead:

```yaml
reliability: D1              # Checkpoint every command (safest, ~5ms overhead)
# reliability: D2            # Batch 3 commands per checkpoint (~2ms overhead)
# reliability: D3            # Checkpoint every 10 commands (~1ms overhead, riskier)
```

**Production default:** `D1` for safety-critical (automotive), `D2` for IoT/smart-home.

---

## 3. Skill Optimization

### Deterministic-First Routing

80% of user commands are repetitive. Route them deterministically to skip the LLM:

```yaml
routing:
  deterministic_first: true          # Try exact pattern match before LLM
  confidence_threshold: 0.95         # Only invoke LLM when uncertain
```

**Impact:** "Turn on AC" → 12ms (deterministic) vs 87ms (LLM-routed).

### Skill Trigger Patterns

More specific patterns = fewer false matches = faster routing:

```yaml
# ❌ Too broad — matches everything
trigger:
  patterns: ["控制", "设置"]

# ✅ Specific — fast rejection
trigger:
  patterns: ["空调", "AC", "air conditioning", "climate"]
```

---

## 4. MCP Service Latency

MCP calls often dominate end-to-end latency. Profile them:

```bash
sparx profile my-agent --mcp-trace
```

### Async MCP Calls

If your agent invokes multiple MCP services, parallelize:

```yaml
# agent.yaml
mcp_services:
  - vehicle.climate
  - vehicle.media
  - vehicle.nav

orchestration:
  parallel_mcp: true         # Call all 3 services concurrently
```

### Local vs. Remote MCP

- **Local MCP** (Unix socket): <5ms overhead
- **Remote MCP** (HTTP): 20-100ms network overhead

**Best practice:** Co-locate MCP servers on the same device whenever possible.

---

## 5. Model Selection

| Model | Context | Latency (SA8650) | Use Case |
|-------|---------|------------------|----------|
| `qwen3-4b` | 2048 | 80-100ms | General-purpose, balanced |
| `qwen3-2b` | 2048 | 50-70ms | Command-and-control, minimal reasoning |
| `llama-3-8b` | 4096 | 150-200ms | Complex reasoning, documents |

```yaml
model:
  id: qwen3-2b               # Smallest that meets your accuracy bar
```

**Accuracy vs. Speed tradeoff:** Test on real user inputs. Often 2B is "good enough" for automotive.

---

## 6. Power Optimization (IoT/Battery Devices)

### Low-Power Mode

```yaml
power:
  mode: low_power
  wake_on: [sensor_threshold, scheduled, manual]
  sleep_after_idle_ms: 5000  # Sleep after 5s inactivity
```

**Impact:** 90% reduction in idle power (3W → 0.3W on SA8650).

### Model Quantization

Sparx ships with INT8-quantized models by default. If you compiled custom models:

```bash
# Use Qualcomm's qairt-converter
qairt-converter --input_network model.onnx \
                --quantization_overrides qnn_int8.json \
                --output_path model_int8.bin
```

---

## 7. Benchmarking

### Built-in Profiler

```bash
sparx profile my-agent --iterations 100
```

Output:
```
Latency Distribution (100 runs):
  p50: 87ms
  p95: 112ms
  p99: 143ms

Breakdown:
  Intent recognition: 45ms (52%)
  NPU inference: 28ms (32%)
  MCP calls: 12ms (14%)
  Other: 2ms (2%)
```

### Continuous Monitoring

In production, log latency per command:

```yaml
telemetry:
  log_latency: true
  alert_threshold_ms: 200    # Alert if >200ms
```

---

## 8. Production Checklist

- [ ] **Context length ≤ 2048** for <100ms target
- [ ] **Deterministic-first routing** enabled
- [ ] **MCP services co-located** (Unix socket preferred)
- [ ] **Reliability level** matches risk tolerance (D1/D2/D3)
- [ ] **Memory retention ≤ 5 entries**
- [ ] **Model = smallest that meets accuracy bar**
- [ ] **Profiled on target hardware** (not dev machine)
- [ ] **Quantization verified** (INT8 for production)

---

## Common Latency Issues

| Symptom | Root Cause | Fix |
|---------|-----------|------|
| 300ms+ on first command | Model loading overhead | Preload with `sparx daemon --preload` |
| Latency grows over session | Memory unbounded | Set `max_entries` in agent.yaml |
| P99 >> P50 | GC pauses in runtime | Use Jemalloc: `LD_PRELOAD=libjemalloc.so sparx run` |
| Spiky latency (50ms → 200ms) | Thermal throttling | Check `cat /sys/class/thermal/thermal_zone*/temp` |

---

## Further Reading

- [QNN Performance Best Practices](https://docs.qualcomm.com/qnn/performance.html)
- [Automotive AI Latency Requirements (ISO 26262)](https://www.iso.org/standard/68388.html)
- [Sparx MCP Service Development Guide](./MCP_SERVICES.md)
