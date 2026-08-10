# ⚡ IoT Edge Agent Example

[English](#english) | [中文](#中文)

---

<a name="english"></a>

Ultra-lightweight agent optimized for battery-powered IoT devices.

## Quick Start

```bash
cd examples/iot_edge
sparx run --low-power

# Agent sleeps until sensor threshold triggers
# Wakes, processes, responds, sleeps again
```

## Features

- **Low-power mode**: Sleeps between events, <1mW standby
- **Sensor-driven**: Wakes on threshold crossing
- **Minimal context**: 2048 tokens, fast inference
- **Edge-optimized**: No cloud, no network dependency

## Use Cases

- Industrial sensor monitoring
- Environmental alerting (temperature, humidity, gas)
- Predictive maintenance triggers
- Agricultural automation

## Architecture

```
Sensor Data Stream
     │
     ▼
┌─────────────┐
│Sleep/Wake Mgr│ ← Wake on threshold crossing
└──────┬──────┘
       ▼
┌─────────────┐
│Skill Dispatch│ ← sensor_monitor / alert / data_sync
└──────┬──────┘
       ▼
┌─────────────┐
│ MCP Service  │ ← iot.sensor / iot.alert
└──────┬──────┘
       ▼
┌─────────────┐
│   Action     │ ← Log, notify, or sync to edge gateway
└──────┬──────┘
       ▼
   Sleep mode (<1mW)

Total wake time: <50ms (ultra-low power)
```

## Files

- `agent.yaml` — Agent configuration with low-power settings
- `skills/sensor_monitor.yaml` — Real-time sensor data processing
- `skills/alert.yaml` — Threshold-based alerting logic
- `skills/data_sync.yaml` — Batch sync to edge gateway

## Example: Temperature Alert

```bash
# Sensor reading: 28.5°C (threshold: 25°C)
# Agent wakes → processes → triggers alert → sleeps

sparx run --low-power

# Output:
# ⚙️  Sensor alert triggered
# ├─ Sensor: temperature = 28.5°C
# ├─ Threshold: 25.0°C exceeded
# ├─ Skill: alert.temperature_high ✓
# ├─ Action: notify_operator [12ms] ✓
# └─ Entering sleep mode...
```

## Customization

Adjust thresholds in `agent.yaml`:

```yaml
low_power:
  sleep_interval_ms: 5000
  wake_on:
    - type: threshold
      sensor: temperature
      condition: "> 25.0"
    - type: threshold
      sensor: humidity
      condition: "> 80.0"
```

---

<a name="中文"></a>

# ⚡ IoT边缘Agent示例

为电池供电IoT设备优化的超轻量Agent。

## 快速开始

```bash
cd examples/iot_edge
sparx run --low-power

# Agent在传感器阈值触发前保持睡眠
# 唤醒、处理、响应、再次睡眠
```

## 特性

- **低功耗模式**：事件间休眠，待机<1mW
- **传感器驱动**：阈值越限时唤醒
- **最小上下文**：2048 tokens，快速推理
- **边缘优化**：无云依赖，无网络需求

## 用例

- 工业传感器监控
- 环境报警（温度、湿度、气体）
- 预测性维护触发
- 农业自动化

## 架构

```
传感器数据流
     │
     ▼
┌─────────────┐
│休眠/唤醒管理│ ← 阈值越限时唤醒
└──────┬──────┘
       ▼
┌─────────────┐
│ 技能分发     │ ← sensor_monitor / alert / data_sync
└──────┬──────┘
       ▼
┌─────────────┐
│ MCP服务      │ ← iot.sensor / iot.alert
└──────┬──────┘
       ▼
┌─────────────┐
│   执行       │ ← 记录、通知或同步到边缘网关
└──────┬──────┘
       ▼
   休眠模式 (<1mW)

总唤醒时间：<50ms（超低功耗）
```

## 文件

- `agent.yaml` — 带低功耗设置的Agent配置
- `skills/sensor_monitor.yaml` — 实时传感器数据处理
- `skills/alert.yaml` — 基于阈值的告警逻辑
- `skills/data_sync.yaml` — 批量同步到边缘网关

## 示例：温度告警

```bash
# 传感器读数：28.5°C（阈值：25°C）
# Agent唤醒 → 处理 → 触发告警 → 休眠

sparx run --low-power

# 输出：
# ⚙️  传感器告警触发
# ├─ 传感器：temperature = 28.5°C
# ├─ 阈值：超过 25.0°C
# ├─ 技能：alert.temperature_high ✓
# ├─ 动作：notify_operator [12ms] ✓
# └─ 进入休眠模式...
```

## 自定义

在`agent.yaml`中调整阈值：

```yaml
low_power:
  sleep_interval_ms: 5000
  wake_on:
    - type: threshold
      sensor: temperature
      condition: "> 25.0"
    - type: threshold
      sensor: humidity
      condition: "> 80.0"
```
