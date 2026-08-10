# 原子服务与 MCP

## 1. Tool 定义

原子能力以 MCP Tool 结构注册：

```json
{
  "name": "com_sgm_service_climate_setAutoFanSpeed",
  "inputSchema": {
    "type": "object",
    "properties": {
      "location": {
        "type": "string",
        "description": "调整自动风速的位置，可选 FRONT、REAR"
      },
      "mode": {
        "type": "string",
        "description": "自动风速目标模式，可选 LOW、NORMAL、HIGH"
      }
    },
    "required": ["location", "mode"],
    "additionalProperties": false
  },
  "description": "设置自动空调风速并返回最终应用结果。",
  "annotations": {
    "title": "setAutoFanSpeed"
  }
}
```

代码同时维护输出 schema、完成策略、资源键、幂等策略、并发上限、抢占能力和可重试错误集合。运行策略不允许由模型提供。

## 2. MCP Wire

`McpProtocolAdapter` 支持受控的 `tools/list` 和 `tools/call` JSON-RPC 形状。适配器负责：

- JSON-RPC 和 method 校验。
- 请求身份与业务调用身份绑定。
- inputSchema 校验。
- MCP result/error 与内部状态映射。
- 危险、重复或多余字段失败关闭。

## 3. 执行语义

执行流程为：

```text
submit -> accepted/rejected -> query/event -> terminal/unknown -> reconcile
```

`accepted` 只代表服务拥有该 execution。Provider 返回前后都校验 execution ID、attempt、provider epoch、lease 和 fencing token。

## 4. 幂等与副作用

- 相同幂等键和相同请求摘要返回同一执行身份。
- 相同幂等键但不同摘要被拒绝。
- Provider 调用前写入 invocation seal。
- 结果丢失后不得自动再次调用 Provider。
- `Unknown` 只能通过 reconcile 接口收敛。

## 5. 抢占

仅 `supports_preemption=true` 且尚未越过不可逆安全点的执行可被抢占。`control_epoch` 必须单调增加，旧抢占命令被拒绝。
