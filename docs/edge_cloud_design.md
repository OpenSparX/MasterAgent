# 端云融合架构设计文档

## Edge-Cloud Dual-Path Inference Pipeline

### 1. 概述

OAK 端云融合架构在原有 100% 端侧推理的基础上，新增一条云端推理链路。
两路并发执行，由本地仲裁层（Arbiter）选取最优结果作为最终输出。

**设计目标：**
- Token 友好：本地 Prompt Engine 压缩后再上云，减少 70-90% token 消耗
- 边界智能最大化：本地能力不足时才触发云端，精确弥补能力短板
- 效率优先：座舱时间敏感场景，deadline 硬约束，绝不阻塞
- 可插拔架构：所有组件均为接口，配置驱动切换，支持多平台拓展

### 2. 数据流

```
User Input
    │
    ▼
┌────────────────┐
│ Speculative    │──── cache hit ───→ 直接返回 (0.02ms)
│ Engine         │
└───────┬────────┘
        │ cache miss
        ▼
┌────────────────────────────┐
│ Confidence Pre-Score       │
│ (启发式快速评估)            │
└───────┬────────────────────┘
        │
   ┌────┴─────────────────────────────┐
   │                                   │
   │  score >= 0.85                    │  score < 0.85
   │  (高置信)                          │  (中/低置信)
   │                                   │
   ▼                                   ▼
┌──────────┐                    ┌─────────────────┐
│ Local    │                    │ Prompt Engine   │
│ Inference│                    │ (压缩 + 蒸馏)    │
│ Only     │                    └────────┬────────┘
└────┬─────┘                             │
     │                                   ▼  async
     │                    ┌─────────────────────────┐
     │                    │ Cloud Backend (HTTP)    │
     │                    └────────────┬────────────┘
     │                                 │
     │    ┌──── Local Inference ───┐   │
     │    └───────────┬────────────┘   │
     │                │                │
     ▼                ▼                ▼
┌─────────────────────────────────────────────┐
│              Arbiter (仲裁层)                 │
│                                              │
│  策略: cloud_prefer | latency_first |        │
│        confidence | local_only               │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
             Final Output
```

### 3. 核心模块

#### 3.1 Prompt Engine (`IPromptEngine`)

**职责：** 上云前压缩提示词，减少 token 消耗

**处理流程：**
1. **Intent Distillation（意图蒸馏）** — 将自然语言转为结构化语义
2. **Context Pruning（上下文剪枝）** — 只保留相关对话历史
3. **Template Rendering（模板填充）** — 用精简模板包装
4. **Token Budget（预算卡控）** — 超预算时进一步压缩

**压缩效果示例：**
```
原始上下文: ~1300 tokens
  System prompt (500) + History 10轮 (800) + User input

压缩后: ~60 tokens
  {"task":"navigation","query":"附近充电桩","vehicle_context":{"soc":23}}
```

**实现类：**
- `CompressedPromptEngine` — 默认压缩引擎（生产用）
- `VerbosePromptEngine` — 全量透传（调试用）

#### 3.2 Cloud Backend (`ICloudBackend`)

**职责：** 纯 HTTP 调用，不加 Agent 逻辑

**支持协议：**
- OpenAI Compatible（覆盖 DeepSeek/Qwen/vLLM/Ollama）
- Anthropic Messages API（预留）
- Custom HTTP（模板化 body）

**特性：**
- 异步非阻塞 (`inferAsync` / `inferWithCallback`)
- 超时严格执行
- Mock 后端用于测试

#### 3.3 Confidence Scorer (`IConfidenceScorer`)

**职责：** 评估本地推理能力，决定是否触发云端

**两阶段评分：**
- **Pre-Score（推理前）：** 基于意图类型 + 历史命中率快速判断
- **Post-Score（推理后）：** 基于 logprob/perplexity 评估输出质量

**置信度门控：**
| 区间 | 行为 |
|:---|:---|
| score > 0.85 | 本地直出，不调云端 |
| 0.4 < score < 0.85 | 端云并发，仲裁选优 |
| score < 0.4 | 云端优先，本地做 fallback |

#### 3.4 Arbiter (`IArbiter`)

**职责：** 接收两路结果，选一个作为最终输出

**策略：**
| 策略 | 逻辑 |
|:---|:---|
| `cloud_prefer` | 两者都到时选云端（默认） |
| `latency_first` | 谁先到用谁 |
| `confidence` | 按 post-score 对比选高者 |
| `local_only` | 强制离线模式 |

**Deadline 机制：**
- 全局 deadline（默认 3000ms）
- Per-intent 覆盖（车控 200ms，导航 500ms，问答 3000ms）
- 超时则用已到的结果，不等另一路

#### 3.5 Pipeline Harness

**职责：** 顶层编排器，组装所有插槽并执行完整流程

**Harness 设计（参考 DeepSeek Harness）：**
```cpp
PipelineHarness harness;
harness.registerPromptEngine("compressed", ...);
harness.registerCloudBackend("openai_compatible", ...);
harness.registerArbiter("cloud_prefer", ...);
harness.registerConfidenceScorer("heuristic", ...);
harness.registerLocalInference("llama_cpp", ...);
harness.loadConfig("config/harness.yaml");

auto response = harness.execute(request);
```

**可插拔性：**
- 所有组件通过接口注册，配置驱动激活
- 运行时热切换（`setActiveArbiter("latency_first")`）
- 新平台只需实现对应 `ILocalInference`

### 4. 配置

配置文件：`config/harness.yaml`

```yaml
harness:
  prompt_engine: "compressed"
  cloud_backend: "openai_compatible"
  arbiter: "cloud_prefer"
  confidence_scorer: "heuristic"
  cloud_enabled: true

cloud:
  provider: "openai_compatible"
  endpoint: ""        # 用户配置
  api_key_env: "SPARX_CLOUD_KEY"
  model: ""           # 用户配置
  timeout_ms: 3000

confidence:
  high_threshold: 0.85
  low_threshold: 0.4
```

### 5. 文件结构

```
cli/include/
├── sparx_pipeline_harness.h     # Harness + ILocalInference 接口
├── sparx_prompt_engine.h        # IPromptEngine 接口 + 实现
├── sparx_cloud_backend.h        # ICloudBackend 接口 + 实现
├── sparx_arbiter.h              # IArbiter 接口 + 实现
└── sparx_confidence_scorer.h    # IConfidenceScorer 接口 + 实现

cli/src/
├── sparx_pipeline_harness.cpp   # Harness 编排逻辑
├── sparx_prompt_engine.cpp      # 意图蒸馏 + 上下文剪枝
├── sparx_cloud_backend.cpp      # HTTP 调用 (curl)
├── sparx_arbiter.cpp            # 仲裁策略实现
└── sparx_confidence_scorer.cpp  # 置信度评估

config/
└── harness.yaml                 # 端云管线配置

templates/
├── default.txt                  # 通用压缩模板
├── navigation.txt               # 导航场景模板
└── vehicle_control.txt          # 车控场景模板

tests/
└── test_harness.cpp             # 15 个单元测试
```

### 6. 构建

```bash
# 标准构建（自动检测 curl）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMASTER_AGENT_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# 运行测试
ctest --test-dir build --output-on-failure
```

### 7. 未来拓展

- [ ] Anthropic 后端实现
- [ ] 自适应仲裁（基于 sparx_learning 在线优化阈值）
- [ ] Streaming 支持（SSE 逐 token 返回）
- [ ] 多模型路由（不同 intent 走不同云端模型）
- [ ] 端侧结果缓存（相似 query 免重复上云）
- [ ] Token 用量监控 + 预算报警
- [ ] 更多平台 ILocalInference 适配（RKNN、MTK NeuroPilot）
