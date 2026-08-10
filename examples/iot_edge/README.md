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
