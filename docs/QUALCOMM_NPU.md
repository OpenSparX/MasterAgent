# Qualcomm NPU Integration

Sparx accelerates inference on Qualcomm NPU hardware via the QNN SDK, delivering sub-100ms latency at low power consumption.

## Architecture

```
┌─────────────────────────────────────────┐
│              Sparx Agent                 │
├─────────────────────────────────────────┤
│         Inference Framework              │
│    ┌────────┐  ┌────────┐  ┌────────┐   │
│    │  Mock  │  │  CPU   │  │  NPU   │   │
│    │ (dev)  │  │(llama) │  │ (QNN)  │   │
│    └────────┘  └────────┘  └────────┘   │
├─────────────────────────────────────────┤
│           QNN SDK (HTP Backend)          │
├─────────────────────────────────────────┤
│         Qualcomm NPU Hardware            │
│    SA8155P / SA8295P / SA8650P           │
└─────────────────────────────────────────┘
```

## Supported Platforms

| Platform | NPU Compute | Status |
|----------|-------------|--------|
| SA8155P | 4 TOPS | ✅ Verified |
| SA8295P | 10 TOPS | ✅ Verified |
| SA8650P | 36 TOPS | ✅ Verified |
| SA8775P | 48 TOPS | 🔄 Testing |
| QCS6490 | 12 TOPS | 🔄 Planned |

## Three Inference Modes

### Mock (Development)

No model needed — returns canned responses. For skill development, testing, and CI.

```yaml
inference:
  backend: mock
```

### CPU (Offline Development)

Runs GGUF models via llama.cpp. For local development without NPU hardware.

```yaml
inference:
  backend: cpu
  model: models/qwen3-4b-q4_k_m.gguf
  threads: 4
  context_size: 4096
```

### NPU (Production)

QNN SDK hardware acceleration. For production deployment.

```yaml
inference:
  backend: npu
  model: models/qwen3-4b.qnn
  platform: SA8295P
  precision: fp16
  kv_cache:
    enabled: true
    max_tokens: 4096
```

## Performance

| Setup | Latency | Power |
|-------|---------|-------|
| Sparx + QNN NPU (SA8295P) | **87ms** | **2.3W** |
| llama.cpp CPU (ARM Neon) | 1,240ms | 8.1W |
| Cloud API (OpenAI) | 2,500ms+ | N/A |

## Two-Stage Inference

Sparx only calls the model when deterministic rules can't close:

```
"Set AC to 22°C"
  → Stage 1: Skill match → climate_control (params closed) → execute directly
  → No model invocation needed

"Plan my schedule for tomorrow"
  → Stage 1: No full match
  → Stage 2: NPU inference [87ms] → parse JSON → execute task DAG
```

Result: 80%+ of requests need zero inference time.

## KV Cache

```yaml
kv_cache:
  enabled: true
  max_tokens: 4096
  eviction: lru
  preload_system: true   # pre-cache system prompt
```

| KV Cache | First inference | Subsequent | Memory |
|----------|----------------|------------|--------|
| Off | 87ms | 87ms | Low |
| On | 87ms | 23ms | +256MB |
| On + preload | 45ms | 23ms | +384MB |

## Power Management

```yaml
inference:
  power:
    mode: balanced        # performance / balanced / low_power
    thermal_throttle: true
    idle_timeout_ms: 5000
```

| Mode | Latency | Power | Use Case |
|------|---------|-------|----------|
| performance | Lowest | ~4.5W | Continuous interaction |
| balanced | Moderate | ~2.3W | General use |
| low_power | Higher | ~1.1W | IoT / battery |

## Model Conversion

```bash
sparx model convert \
  --input models/qwen3-4b.gguf \
  --output models/qwen3-4b.qnn \
  --platform SA8295P \
  --precision fp16

sparx model verify models/qwen3-4b.qnn
```

> **Note**: NPU mode requires the Qualcomm QNN SDK, which is not included in the Sparx open-source distribution. Contact Qualcomm for SDK access.

## Troubleshooting

| Issue | Cause | Fix |
|-------|-------|-----|
| "NPU not found" | Driver not loaded | `modprobe qnn_npu` |
| "Model format error" | QNN version mismatch | Re-convert with matching SDK |
| "Out of memory" | NPU memory exhausted | Reduce context_size or use int4 |
| Latency spikes | Thermal throttling | Check cooling or lower freq |

## See Also

- [推理与KV Cache](07_推理与KVCache.md) — Technical internals
- [Performance Tuning](PERFORMANCE_TUNING.md) — Full optimization guide
- [Qualcomm NPU（中文详细版）](QUALCOMM_NPU_zh-CN.md)
