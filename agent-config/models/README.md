# Models

ModelProfile 描述模型身份、角色、运行位置、Runtime、外部引用、推理参数、协议和兼容性门禁。模型文件和凭证不放入配置包：

- 本地权重使用 `path_ref` 环境变量或 CLI 显式覆盖；当前 llama.cpp 兼容变量是 `SPARX_MODEL`。
- 云端 endpoint、模型名和凭证使用 `*_ref` 环境变量。
- 日志只能记录引用名称和摘要，不能记录凭证值。

官方包提供：当前真实存在的本地 Mock intent、确定性 Mock classifier、用户正在使用的 Qwen GGUF 配置，以及默认关闭的 Fake Cloud 和 OpenAI-compatible 云配置。Qwen2.5-3B 当前标记为 `experimental`：Runtime 真实链路已验证，但严格两阶段 Decision JSON 协议的遵从率尚未证明达标。

ModelProfile 只说明“这个模型怎么运行”；选择顺序、端云切换、授权和 deadline 预算由 `policies/model-routing.yaml` 决定。

模型不是与 MasterAgent 的业务架构强绑定，但也不是任意模型文件都能直接互换。替换模型至少要同时满足：存在对应 Runtime Adapter、支持配置包声明的上下文长度、能遵守 Decision JSON 协议、通过配置包引用的协议测试。业务执行能力仍由 Capability/Agent 目录决定，不能由模型自行增加。

显式选择的模型无法解析时必须失败，不能静默换成 Mock。Mock 只允许 development/test，并且必须输出 `SIMULATED` 标记。
