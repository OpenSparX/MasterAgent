# MCP服务集成指南

MCP（Model Context Protocol）是Sparx连接外部能力的标准协议。通过MCP，Agent可以调用车控、导航、智能家居等原子服务。

## 概述

```
Agent
  │
  ▼
┌─────────────────┐
│  MCP Registry   │ ← 服务注册表
└────────┬────────┘
         │
    ┌────┼────┐
    ▼    ▼    ▼
┌──────┐┌──────┐┌──────┐
│车控   ││导航   ││家居   │ ← 原子服务
└──────┘└──────┘└──────┘
```

## 核心概念

### 原子服务

每个MCP服务是一个**原子操作**：
- 有明确的输入/输出schema
- 有幂等性保证（通过idempotency_key）
- 有超时和重试策略
- 有WAL集成（副作用追踪）

### 服务注册

```yaml
# mcp_services.yaml
services:
  - name: vehicle.climate
    description: "车辆空调控制"
    schema:
      input:
        temperature: {type: float, range: [16, 32]}
        mode: {type: enum, values: [heat, cool, auto]}
        fan_speed: {type: int, range: [1, 7]}
      output:
        success: {type: bool}
        current_temp: {type: float}
    timeout_ms: 5000
    retry:
      max_attempts: 2
      backoff_ms: 1000
    side_effect: true
    idempotent: true
```

### 服务调用流程

```
1. Skill/Intent识别 → 确定需要调用的服务
2. 参数验证 → 检查schema合规
3. WAL写入 → 记录待执行操作
4. 执行调用 → 通过MCP协议
5. 结果确认 → 更新WAL状态
6. 返回结果 → 组装响应
```

## 添加自定义MCP服务

### 步骤1：定义服务Schema

创建`services/my_service.yaml`：

```yaml
name: home.lights
version: "1.0"
description: "智能灯光控制"

methods:
  - name: set_state
    description: "设置灯光状态"
    input:
      room: {type: string, required: true}
      state: {type: enum, values: [on, off], required: true}
      brightness: {type: int, range: [0, 100], default: 100}
      color_temp: {type: int, range: [2700, 6500], default: 4000}
    output:
      success: {type: bool}
      actual_brightness: {type: int}
    side_effect: true
    idempotent: true
    timeout_ms: 3000

  - name: get_state
    description: "获取灯光状态"
    input:
      room: {type: string, required: true}
    output:
      state: {type: enum, values: [on, off]}
      brightness: {type: int}
    side_effect: false
    timeout_ms: 1000
```

### 步骤2：实现服务处理器

```cpp
// src/services/home_lights.h
#pragma once
#include "mcp/service_base.h"

class HomeLightsService : public mcp::ServiceBase {
public:
    std::string name() const override { return "home.lights"; }

    mcp::Result handle(const mcp::Request& req) override {
        if (req.method == "set_state") {
            auto room = req.param<std::string>("room");
            auto state = req.param<std::string>("state");
            auto brightness = req.param<int>("brightness", 100);

            // 实际设备控制逻辑
            bool ok = device_bridge_.set_light(room, state, brightness);

            return mcp::Result{
                {"success", ok},
                {"actual_brightness", brightness}
            };
        }
        // ... 其他方法
    }
};
```

### 步骤3：注册服务

在`agent.yaml`中注册：

```yaml
mcp:
  services:
    - path: services/home_lights.yaml
    - path: services/home_thermostat.yaml
    - path: services/vehicle_climate.yaml
```

### 步骤4：关联Skill

在Skill中引用MCP服务：

```yaml
# skills/lights_control.yaml
name: lights_control
handler:
  type: deterministic
  service: home.lights
  method: set_state
  parameter_mapping:
    room: "{entities.room}"
    state: "{entities.action}"
    brightness: "{entities.level}"
```

## 内置MCP服务

| 服务 | 描述 | 适用场景 |
|------|------|---------|
| `vehicle.climate` | 空调/暖风控制 | 车载 |
| `vehicle.media` | 媒体播放控制 | 车载 |
| `vehicle.navigation` | 导航目的地设置 | 车载 |
| `vehicle.phone` | 电话/通讯录 | 车载 |
| `home.lights` | 灯光控制 | 智能家居 |
| `home.thermostat` | 温控器 | 智能家居 |
| `home.security` | 安防系统 | 智能家居 |
| `iot.sensor` | 传感器读取 | IoT边缘 |
| `iot.alert` | 告警触发 | IoT边缘 |
| `iot.data_sync` | 数据同步 | IoT边缘 |

## 错误处理

MCP服务的错误通过标准错误码返回：

```yaml
error_codes:
  TIMEOUT: 服务调用超时
  UNAVAILABLE: 服务不可用
  INVALID_PARAMS: 参数校验失败
  DEVICE_ERROR: 底层设备错误
  PERMISSION_DENIED: 权限不足
  UNKNOWN: 无法确定结果（触发WAL Unknown流程）
```

### 错误处理策略

```yaml
error_handling:
  timeout:
    action: retry
    max_attempts: 2
  unavailable:
    action: fallback
    fallback_service: "mock.{service_name}"
  unknown:
    action: wal_unknown
    notify: operator
```

## 并发与优先级

MCP服务支持优先级调度：

| 优先级 | 场景 | 抢占 |
|--------|------|------|
| P0 | 安全关键（紧急制动、烟雾报警） | 立即抢占 |
| P1 | 用户交互（空调、导航） | 队列头部 |
| P2 | 后台任务（数据同步、日志上传） | 可被抢占 |

```yaml
# 高优先级服务配置
services:
  - name: vehicle.emergency
    priority: P0
    preempt: true
    timeout_ms: 100    # 极短超时
```

## 参考

- [原子服务与MCP](06_原子服务与MCP.md) — 技术细节
- [任务编排](05_任务编排.md) — DAG调度
- [WAL恢复机制](WAL_RECOVERY_zh-CN.md) — 崩溃恢复
