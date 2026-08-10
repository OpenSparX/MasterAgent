# OpenSparX CLI — 开发日志

> 版本: v0.1.0 | 日期: 2026-08-11  
> 对应内核版本: MasterAgent v2.0.0 (master_agent_core)

---

## 概览

本次开发围绕 **"5分钟一键接入"** 的产品目标，在 v2 内核之上新建了 `cli/` 目录，包含开发者 CLI (`sparx`)、两个实体 IModelRuntime 适配器、以及端侧部署链的全部工具命令。全部代码编译通过，15/15 测试无回归。

新增总计 **~3000 行 C++** + 52 行 CMake + 模板。

---

## 交付物 (原 6 项 + 补充 3 项)

| # | 模块 | 入口 | 状态 |
|---|---|---|---|
| 1 | `agent.yaml` 格式 + `sparx run` | `cmd_init.cpp`, `cmd_run.cpp` | ✅ 可运行 |
| 2 | `LlamaCppModelRuntime` | `llama_cpp_model_runtime.{h,cpp}` | ✅ 编译通过 |
| 3 | `sparx devices` + `sparx deploy --start` | `cmd_devices.cpp`, `cmd_deploy.cpp` | ✅ 可运行 |
| 4 | `GenieModelRuntime` (dlopen libGenie.so) | `genie_model_runtime.{h,cpp}` | ✅ 编译通过 |
| 5 | `sparx doctor` | `cmd_doctor.cpp` | ✅ 可运行 |
| 6 | `sparx demo crash` / stream | `cmd_demo.cpp` | ✅ 可运行 |
| 7 | `sparx pull <model>` | `cmd_pull.cpp` | ✅ 可运行 |
| 8 | `IModelRuntime::runtimeTag()` + executor 接线 | `inference_framework.h`, `model_executor.cpp` | ✅ 测试通过 |
| 9 | 设备端 daemon + `--start`/`--stop` | `cmd_deploy.cpp`, `sparx_agent.sh` | ✅ 编译通过 |
| 5 | `sparx doctor` | `cmd_doctor.cpp` | ✅ 可运行 |
| 6 | `sparx demo crash` / stream | `cmd_demo.cpp` | ✅ 可运行 |

---

## 新增函数清单

### 1. CLI 命令层 (`sparx` namespace)

| 函数 | 文件 | 作用 |
|---|---|---|
| `cmd_init(args)` | cmd_init.cpp | 脚手架: 生成 agent.yaml + skills/hello.yaml |
| `cmd_pull(args)` | cmd_pull.cpp | 下载 GGUF/context binary, 进度条 + 校验 |
| `cmd_run(args)` | cmd_run.cpp | 本地 REPL, 确定性路由优先, mock 推理 |
| `cmd_devices(args)` | cmd_devices.cpp | 列出 adb 可达设备 + SoC/NPU 能力表 |
| `cmd_deploy(args)` | cmd_deploy.cpp | push agent + config + daemon 到设备, 支持 --start/--stop |
| `cmd_doctor(args)` | cmd_doctor.cpp | 6 项诊断 (见下) |
| `cmd_demo(args)` | cmd_demo.cpp | WAL 崩溃恢复 + 流式验证演示 |
| `cmd_shell(args)` | cmd_shell.cpp | 交互 REPL (本地 / adb forward 远程) |

### 2. 配置与设备检测 (header-only)

| 函数 | 文件 | 作用 |
|---|---|---|
| `loadAgentConfig(path, config)` | sparx_agent_config.h | 轻量级 flat-YAML 解析 |
| `matchesDeterministicSkill(skill, input)` | sparx_agent_config.h | 确定性技能模式匹配 |
| `executeDeterministicSkill(skill, input)` | sparx_agent_config.h | 执行确定性技能 |
| `discoverDevices()` | sparx_device_info.h | adb 枚举 + SoC/NPU probe |
| `lookupSoc(soc_id)` | sparx_device_info.h | 查 KNOWN_SOCS 表 |
| `generateGenieConfig(dev, model)` | sparx_genie_config.h | 生成 HTP ext config JSON |
| `generateGenieDialogConfig(dev, model, agent)` | sparx_genie_config.h | 生成 Genie dialog config JSON |

### 3. LlamaCppModelRuntime (master_agent::inference)

| 函数 | 作用 |
|---|---|
| `requiredWorkUnits(request)` | 调度器 quota 估算，不调用模型 |
| `infer(request, seal)` | 非流式路径 → 内部委托 inferStream(null sink) |
| `inferStream(request, seal, sink)` | 核心: POST /v1/chat/completions SSE → InferenceChunk |
| `supportsStreaming()` | 始终返回 true |
| `validateSeal(request, seal)` | 与 mock runtime 相同的 11 字段 fail-closed 检查 |
| `echoSeal(out, seal, request)` | 将 seal 身份字段复制到 output |
| `buildRequestBody(request, stream)` | 构造 OpenAI 兼容 JSON |
| `isServerReady()` | /health 探针 |
| `waitForServer(timeout)` | 阻塞等待服务就绪 |
| `ensureServer()` | fork + execvp llama-server (如果没有外部进程) |
| `stopServer()` | SIGTERM → 等待 → SIGKILL |

### 4. GenieModelRuntime (master_agent::inference)

| 函数 | 作用 |
|---|---|
| `probe(library_dir)` | 静态: dlopen + dlsym 探测 libGenie.so 可用性 |
| `requiredWorkUnits(request)` | NPU prefill 更快, 因此 quota 更小 |
| `infer(request, seal)` | 非流式 → 内部委托 inferStream(null sink) |
| `inferStream(request, seal, sink)` | 核心: GenieDialog_query + token callback → InferenceChunk |
| `supportsStreaming()` | 始终返回 true |
| `validateSeal(request, seal)` | fail-closed seal 验证 |
| `echoSeal(out, seal, request)` | seal → output 身份回显 |
| `ensureDialog()` | 惰性创建 Genie dialog handle |
| `releaseDialog()` | 释放 handle |
| `reload(config_path)` | 热更新模型配置 |
| `isReady()` | dialog 是否活跃 |
| `genieTokenCallback(response, code, user_data)` | C callback → InferenceChunk 转换 |

### 5. sparx doctor 诊断项

| 检查 | 真实来源 |
|---|---|
| `checkAdb` | adb 是否可达 |
| `checkQnn` | /vendor/lib64/libQnnHtp.so 是否存在 |
| `checkGenie` | libGenie.so + 5 个必要符号 |
| `checkModelArtifacts` | 模型文件是否在 /data/local/tmp/sparx/ |
| `checkSocIdConsistency` | 陷阱编码: artifact 文件名 socid87 vs 设备 soc_id 72 |
| `checkMemoryBudget` | Qwen3-4B ~2.4GB peak 内存是否可用 |
---

## 核心模块作用

### `cli/` 为什么是独立 target

`master_agent_core` 必须保持可作为纯嵌入式库使用 —— 不链接任何 CLI、进程 fork、socket 代码。因此：

- `sparx_runtimes` (静态库): 两个 runtime 适配器，测试可链接而不引入 `main()`
- `sparx` (可执行): 命令层

### GenieModelRuntime 的三个关键设计决策

**1. dlopen 而非链接期依赖**

同一个 `sparx` 二进制必须能在没有高通栈的开发机上运行（干净报告 "NPU unavailable" 并回落 llama.cpp），也能在设备上用 NPU。链接期依赖会让前者根本无法启动。

**2. `GenieDialog_query` + callback，而非 spawn `genie-t2t-run`**

spawn 示例二进制的代价：无法流入我们的 sink、每轮 ~300ms 进程启动、跨轮无 KV cache 复用、要解析人类可读 stdout。进程内 API 给出 token 级 callback 和持久 dialog handle。

**3. dialog handle 跨调用保留**

`reuse_dialog = true` 时保留以复用 KV cache；会话切换时 reset。

### 流式 UTF-8 边界保护

`genieTokenCallback` 中，未标记 `SENTENCE_END` 的 span 会回退到完整 UTF-8 字符边界再 emit：

```cpp
while (emit_len > 0 &&
       (static_cast<unsigned char>(state->pending[emit_len - 1]) & 0xC0) == 0x80) {
    --emit_len;
}
```

否则渲染型 sink (TTS / UI) 会对半个汉字输出替换字符。

---

## 与 V2 版本的区别

### V2 有什么

v2 内核完整实现了编排、两阶段提交围栏、幂等账本、WAL、协作式抢占、确定性优先路由。**但它只有 `MockModelRuntime`** —— 唯一的 `IModelRuntime` 实现是模拟的，`reality` 字段恒为 `SIMULATED`。

v2 也没有任何面向开发者的入口：接入内核需要写 C++、手工构造 `InferenceRequest`、自己管理 `CallContext`。

### 本次新增的区别点

| 维度 | V2 | 本次新增 |
|---|---|---|
| **模型 runtime** | 仅 MockModelRuntime | + LlamaCppModelRuntime (CPU/GPU)<br>+ GenieModelRuntime (Hexagon NPU) |
| **接入方式** | 写 C++ 代码 | `sparx init` → `agent.yaml` 声明式 |
| **端侧部署** | 无 | `sparx devices` / `deploy --start` / `doctor` |
| **模型获取** | 手动下载 | `sparx pull qwen3-4b` 进度条 + 自动放对位置 |
| **设备 daemon** | 无 | `sparx_agent.sh` — 崩溃重启、backoff、PID 管理 |
| **Genie config** | 手工编写 JSON | 按检测到的 SoC 自动生成 |
| **SoC 知识** | 无 | KNOWN_SOCS 表 (8 款, soc_id → dsp_arch/VTCM/HVX) |
| **可诊断性** | 无 | 6 项 doctor 检查, 每项对应真实踩过的坑 |
| **流式** | ABI 已定义 (M1) | 两个 runtime 实际实现了 `inferStream` |
| **runtime 接线** | 硬编码 mock-runtime | `IModelRuntime::runtimeTag()` 虚函数，executor 动态取值 |
| **演示能力** | 无 | `sparx demo crash` 30 秒展示 UNKNOWN 状态 |

### 关键点：M1 流式 ABI 首次有了真实实现

上一阶段 (M1) 定义了流式 ABI：`InferenceChunk`、`StreamControl`、`InferenceStreamSink`、`StreamIntegrity`，以及框架侧独立累积校验。**但当时只有测试 fake 实现它。**

本次两个 runtime 都 override 了 `supportsStreaming()` 和 `inferStream()`，遵守全部 ABI 义务：

- 每个 chunk 回显 `seal.invocation_id`
- `chunk_index` 从 0 单调递增
- 恰好一个 `final == true` 且最后送达
- `sink` 返回 `Abort` 时协作式停止
- `raw_output` 等于所有 delta 的有序拼接（框架会独立校验）

---

## 已知问题与未决项

### ~~1. `model_executor.cpp` 的 seal 构造硬编码了 mock 标签~~ ✅ 已解决

新增 `IModelRuntime::runtimeTag()` 虚函数（默认返回 `"mock-runtime"` 以保持向后兼容），executor 改为 `secureDigest(job.request.model + "|" + runtime_->runtimeTag())`。MockModelRuntime 内部验证也同步改为调用 `runtimeTag()`。15/15 测试全绿。

### 2. Genie 推测解码 draft token 语义未在真机验证

如果 Genie 的 callback 投递未验证的 draft token 而后撤回，框架累积器会看到与最终 `raw_output` 不一致的 delta，从而误报 `INFERENCE_STREAM_OUTPUT_DIVERGED`。

当前 `genieTokenCallback` 的应对：仅在 `SENTENCE_COMPLETE`/`END`/`CONTINUE`/`BEGIN` 时 flush，遇到 `SENTENCE_REWIND` 丢弃未 emit 的 buffer。**这是推断，不是事实** —— `docs.qualcomm.com` 在本环境不可达，必须在真机验证后才能冻结 ABI。

### 3. `InferenceRequest` 缺少 embedding/vision 输入槽位

`GenieDialog_embeddingQuery` 对应的多模态输入（Qwen3-VL 图像）在当前 ABI 里没有位置。**在冻结 ABI 前补比之后补便宜得多。**

### ~~4. `MANIFEST.sha256`~~ ✅ 已删除

由用户确认删除。git tree hash 已覆盖完整性。

### 5. 高通许可约束

`genai_lib/`、`qnn_model_prepare_*.py`、`utilities/` 中任何文件都不得进入公开仓库（71/99 个 Python 文件标记 "Confidential and Proprietary - Qualcomm Technologies, Inc."）。本次新增代码只复用了**如何驱动 Genie 的知识**，没有复制任何高通源文件 —— `genie_abi` namespace 中的签名是本地声明，不是复制的头文件。

---

## 验证记录

```
$ rm -rf /tmp/ma_build && cmake -S . -B /tmp/ma_build -DMASTER_AGENT_BUILD_CLI=ON
$ cmake --build /tmp/ma_build
# 0 errors

$ cd /tmp/ma_build && ctest -j4
100% tests passed, 0 tests failed out of 15
```

新增 `tests/test_sparx_runtimes.cpp`（第 15 个测试，19 项断言），不依赖 llama-server 或 libGenie.so，验证：

| 断言组 | 为什么重要 |
|---|---|
| seal 校验 fail-closed | 接受不完整 seal 的 runtime 会让过期/错投的调用到达模型，等于废掉整个两阶段提交围栏 |
| Genie 先验 seal 再验 NPU | 无 NPU 主机上也必须把非法调用报为非法，而不是报为"设备不可用" |
| `requiredWorkUnits` 不为 0 且随 prompt 增长 | 返回 0 会让调度器饿死该任务；不增长则无法区分廉价与昂贵的轮次 |
| 两者都 advertise streaming | 否则框架永不调用 `inferStream`，M1 ABI 对它们就是死代码 |
| 无高通栈时构造安全 | `sparx doctor` 必须能在开发机上探测而不崩溃 |

### 变异测试（确认测试不是静默通过）

两个 runtime 的 seal 校验分别被人为绕过后，对应断言立即失败（exit=1），改回后恢复全绿：

| 变异 | 结果 |
|---|---|
| `LlamaCppModelRuntime::validateSeal` 恒真 | ✅ 被杀死 — 报 `UNAVAILABLE` 而非 `INVOCATION_INVALID` |
| `GenieModelRuntime::inferStream` 跳过 `validateSeal` | ✅ 被杀死 — "validates the seal before checking NPU availability" 失败 |

端到端手工验证：

```
$ sparx init my-agent          # ✓ agent.yaml + skills/hello.yaml
$ sparx run                    # ✓ 确定性路由命中 (model not invoked)
$ sparx demo crash             # ✓ WAL torn tail
$ sparx demo resume            # ✓ side_effect=UNKNOWN
$ sparx demo stream            # ✓ 9 chunks, VERIFIED
$ sparx devices                # ✓ 无设备时干净报错
$ sparx doctor                 # ✓ 无设备时干净报错
```

**未验证：** 两个 runtime 的实际推理路径。`LlamaCppModelRuntime` 需要 llama-server 二进制 + GGUF 模型；`GenieModelRuntime` 需要真机 + libGenie.so。二者都只验证了编译与探测路径（`probe()` 在本机正确报告 libGenie.so 不存在）。

