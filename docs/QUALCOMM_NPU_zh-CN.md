# Qualcomm NPU集成指南

Sparx通过Qualcomm QNN SDK实现硬件加速推理，在NPU上运行模型实现低延迟、低功耗的Agent推理。

## 概述

```
┌─────────────────────────────────────────┐
│              Sparx Agent                 │
├─────────────────────────────────────────┤
│         Inference Framework              │
│    ┌────────┐  ┌────────┐  ┌────────┐   │
│    │ Mock   │  │  CPU   │  │  NPU   │   │
│    │(开发用) │  │(llama) │  │ (QNN)  │   │
│    └────────┘  └────────┘  └────────┘   │
├─────────────────────────────────────────┤
│           QNN SDK (HTP Backend)          │
├─────────────────────────────────────────┤
│         Qualcomm NPU Hardware            │
│    SA8155P / SA8295P / SA8650P           │
└─────────────────────────────────────────┘
```

## 支持的平台

| 平台 | SoC | NPU算力 | 状态 |
|------|-----|---------|------|
| SA8155P | Snapdragon SA8155 | 4 TOPS | ✅ 已验证 |
| SA8295P | Snapdragon SA8295 | 10 TOPS | ✅ 已验证 |
| SA8650P | Snapdragon SA8650 | 36 TOPS | ✅ 已验证 |
| SA8775P | Snapdragon SA8775 | 48 TOPS | 🔄 测试中 |
| QCS6490 | QCS6490 | 12 TOPS | 🔄 规划中 |

## 推理模式

Sparx提供三种推理后端：

### 1. Mock模式（开发调试）

不需要任何模型，返回预设响应。适用于：
- 技能开发和测试
- CI/CD流水线
- 不依赖模型的逻辑验证

```yaml
# agent.yaml
inference:
  backend: mock
```

### 2. CPU模式（离线开发）

使用llama.cpp在CPU上运行GGUF模型。适用于：
- 本地开发调试
- 无NPU硬件的环境
- 模型效果验证

```yaml
inference:
  backend: cpu
  model: models/qwen3-4b-q4_k_m.gguf
  threads: 4
  context_size: 4096
```

### 3. NPU模式（生产部署）

通过QNN SDK在Qualcomm NPU上运行。适用于：
- 生产环境部署
- 低延迟要求
- 低功耗场景

```yaml
inference:
  backend: npu
  model: models/qwen3-4b.qnn
  platform: SA8295P
  precision: fp16     # fp16 / int8 / int4
  kv_cache:
    enabled: true
    max_tokens: 4096
```

## 模型转换

### 从GGUF到QNN格式

```bash
# 1. 获取GGUF模型
# 例如 Qwen3-4B-Instruct-Q4_K_M.gguf

# 2. 转换为QNN格式（需要QNN SDK）
sparx model convert \
  --input models/qwen3-4b.gguf \
  --output models/qwen3-4b.qnn \
  --platform SA8295P \
  --precision fp16

# 3. 验证模型
sparx model verify models/qwen3-4b.qnn

# 输出：
# ✓ Model format: QNN (valid)
# ✓ Target platform: SA8295P
# ✓ Precision: fp16
# ✓ Context size: 4096 tokens
# ✓ Estimated latency: ~87ms
```

### 支持的模型

| 模型 | 参数量 | NPU延迟 | 推荐场景 |
|------|--------|---------|---------|
| Qwen3-4B | 4B | ~87ms | 通用对话 |
| Qwen2-4B | 4B | ~92ms | 中文优化 |
| Qwen3-1.5B | 1.5B | ~35ms | 简单指令 |
| 自定义 | <7B | 可变 | 特定领域 |

> **注意**：NPU模式需要Qualcomm QNN SDK授权。SDK不包含在Sparx开源发行版中。

## 性能优化

### KV Cache配置

KV Cache可以显著减少重复计算：

```yaml
inference:
  kv_cache:
    enabled: true
    max_tokens: 4096
    eviction: lru        # lru / fifo
    preload_system: true # 预加载system prompt
```

**性能对比**：

| KV Cache | 首次推理 | 后续推理 | 内存占用 |
|----------|---------|---------|---------|
| 关闭 | 87ms | 87ms | 低 |
| 开启 | 87ms | 23ms | +256MB |
| 开启+预加载 | 45ms | 23ms | +384MB |

### 批处理优化

对于IoT场景的批量传感器数据：

```yaml
inference:
  batching:
    enabled: true
    max_batch_size: 8
    max_wait_ms: 10
    dynamic: true        # 动态batch大小
```

### 功耗管理

```yaml
inference:
  power:
    mode: balanced       # performance / balanced / low_power
    thermal_throttle: true
    max_npu_freq_mhz: 1000
    idle_timeout_ms: 5000  # NPU空闲后降频
```

| 模式 | 延迟 | 功耗 | 适用场景 |
|------|------|------|---------|
| performance | 最低 | ~4.5W | 持续交互 |
| balanced | 适中 | ~2.3W | 一般使用 |
| low_power | 较高 | ~1.1W | IoT/电池 |

## 两阶段推理

Sparx的独特设计：仅在确定性规则无法闭合时才调用模型。

```
用户输入: "空调调到22度"

Stage 1: 确定性匹配
├─ Skill: climate_control ✓
├─ 参数: temperature=22, unit=celsius ✓
└─ 结果: 完全闭合 → 直接执行（无需模型）

用户输入: "帮我安排一下明天的行程"

Stage 1: 确定性匹配
├─ Skill: 无完全匹配
└─ 结果: 需要模型 → 进入Stage 2

Stage 2: NPU推理
├─ 组装prompt（含上下文、记忆、可用服务）
├─ NPU推理 [87ms]
├─ 解析JSON协议输出
└─ 执行Task DAG
```

这种设计确保：
- 80%+的请求无需模型调用（零延迟）
- 模型仅处理真正需要推理的场景
- NPU利用率最优

## 部署步骤

### 1. 环境准备

```bash
# 确认硬件平台
sparx doctor --check npu

# 输出：
# ✓ Platform: SA8295P
# ✓ QNN SDK: v2.20.0
# ✓ NPU status: available
# ✓ Driver: v1.8.2
# ✓ Memory: 2048MB available for NPU
```

### 2. 模型部署

```bash
# 转换并验证模型
sparx model convert --input qwen3-4b.gguf --output qwen3-4b.qnn --platform SA8295P
sparx model verify qwen3-4b.qnn

# 部署模型到设备
sparx deploy --model qwen3-4b.qnn --device 1
```

### 3. 运行验证

```bash
# 运行benchmark
sparx benchmark --model qwen3-4b.qnn --iterations 100

# 输出：
# Model: qwen3-4b.qnn
# Platform: SA8295P (NPU)
# Iterations: 100
# ━━━━━━━━━━━━━━━━━━━━
# Avg latency:  87ms
# P50:          82ms
# P95:          112ms
# P99:          138ms
# Throughput:   11.5 req/s
# Power:        2.3W avg
```

## 故障排除

| 问题 | 原因 | 解决 |
|------|------|------|
| "NPU not found" | 驱动未加载 | `modprobe qnn_npu` |
| "Model format error" | QNN版本不匹配 | 使用匹配版本重新转换 |
| "Out of memory" | NPU内存不足 | 降低context_size或使用int4 |
| "Thermal throttle" | 温度过高 | 检查散热或降低频率 |
| 延迟抖动大 | 其他进程占用NPU | 设置独占模式 |

## 参考

- [推理与KV Cache](07_推理与KVCache.md) — 推理框架技术细节
- [性能调优](PERFORMANCE_TUNING.md) — 全面性能指南
- [生产部署](PRODUCTION_DEPLOYMENT.md) — 部署清单
