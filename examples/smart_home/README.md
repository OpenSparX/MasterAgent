# 🏠 Smart Home Agent Example

[English](#english) | [中文](#中文)

---

<a name="english"></a>

Complete smart home automation agent with natural language control.

## Quick Start

```bash
cd examples/smart_home
sparx run

# Natural language commands:
# > Turn on living room lights
# > Set thermostat to 23°C, eco mode
# > Arm security system, away mode
```

## Features

- **Multi-device control**: Lights, thermostat, security, appliances
- **Scene automation**: "Good morning", "Good night", "Away" scenes
- **Energy optimization**: Smart scheduling based on occupancy
- **Voice + text**: Works with both input modes

## Supported Commands

| Category | Example Commands |
|----------|-----------------|
| Lights | "Turn on living room lights" / "Dim bedroom to 30%" |
| Thermostat | "Set temperature to 22°C" / "Enable eco mode" |
| Security | "Arm security system" / "Show camera feed" |
| Scenes | "Activate good morning scene" / "Away mode" |

## Architecture

```
User Command
     │
     ▼
┌─────────────┐
│ NL Parser    │ ← Extract intent + parameters
└──────┬──────┘
       ▼
┌─────────────┐
│Skill Dispatch│ ← lights / thermostat / security
└──────┬──────┘
       ▼
┌─────────────┐
│ MCP Service  │ ← home.lights / home.hvac / home.security
└──────┬──────┘
       ▼
┌─────────────┐
│Device Control│ ← Zigbee/Z-Wave/Matter bridge
└─────────────┘

Response latency: <150ms
```

## Files

- `agent.yaml` — Agent configuration and device registry
- `skills/lights.yaml` — Light control (on/off/dim/color)
- `skills/thermostat.yaml` — HVAC control and scheduling
- `skills/security.yaml` — Security system and cameras

## Example: Scene Automation

```bash
sparx run
> Activate good morning scene

# Output:
# 🏠 Scene: Good Morning
# ━━━━━━━━━━━━━━━━━━━━
# ├─ Lights: bedroom OFF, living_room ON (80%) ✓
# ├─ Thermostat: 22°C, comfort mode ✓
# ├─ Blinds: open ✓
# └─ Coffee maker: start [142ms] ✓
```

## Customization

Define custom scenes in `agent.yaml`:

```yaml
scenes:
  - name: movie_time
    devices:
      - type: lights
        room: living_room
        state: dim
        brightness: 20
      - type: lights
        room: kitchen
        state: off
      - type: thermostat
        temperature: 21
```

---

<a name="中文"></a>

# 🏠 智能家居Agent示例

完整的智能家居自动化Agent，支持自然语言控制。

## 快速开始

```bash
cd examples/smart_home
sparx run

# 自然语言命令：
# > 打开客厅灯
# > 设置温度23度，节能模式
# > 开启安防系统，离家模式
```

## 特性

- **多设备控制**：灯光、温控、安防、电器
- **场景自动化**："早安"、"晚安"、"离家"场景
- **能效优化**：基于占用情况的智能调度
- **语音+文字**：支持两种输入模式

## 支持的命令

| 类别 | 示例命令 |
|------|---------|
| 灯光 | "打开客厅灯" / "卧室灯调到30%" |
| 温控 | "设置温度22度" / "开启节能模式" |
| 安防 | "启动安防系统" / "显示摄像头画面" |
| 场景 | "激活早安场景" / "离家模式" |

## 架构

```
用户命令
     │
     ▼
┌─────────────┐
│ 自然语言解析│ ← 提取意图和参数
└──────┬──────┘
       ▼
┌─────────────┐
│ 技能分发     │ ← lights / thermostat / security
└──────┬──────┘
       ▼
┌─────────────┐
│ MCP服务      │ ← home.lights / home.hvac / home.security
└──────┬──────┘
       ▼
┌─────────────┐
│设备控制      │ ← Zigbee/Z-Wave/Matter网桥
└─────────────┘

响应延迟：<150ms
```

## 文件

- `agent.yaml` — Agent配置和设备注册表
- `skills/lights.yaml` — 灯光控制（开关/调光/颜色）
- `skills/thermostat.yaml` — 空调控制和调度
- `skills/security.yaml` — 安防系统和摄像头

## 示例：场景自动化

```bash
sparx run
> 激活早安场景

# 输出：
# 🏠 场景：早安
# ━━━━━━━━━━━━━━━━━━━━
# ├─ 灯光：卧室关闭，客厅开启(80%) ✓
# ├─ 温控：22°C，舒适模式 ✓
# ├─ 窗帘：打开 ✓
# └─ 咖啡机：启动 [142ms] ✓
```

## 自定义

在`agent.yaml`中定义自定义场景：

```yaml
scenes:
  - name: movie_time
    devices:
      - type: lights
        room: living_room
        state: dim
        brightness: 20
      - type: lights
        room: kitchen
        state: off
      - type: thermostat
        temperature: 21
```
