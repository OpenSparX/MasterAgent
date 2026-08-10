# 🚗 Automotive Voice Assistant Example

[English](#english) | [中文](#中文)

---

<a name="english"></a>

A complete on-device voice assistant for automotive use, demonstrating:

- **Deterministic skill routing** — pattern matching without model calls
- **MCP service integration** — vehicle climate, navigation, media control
- **Bilingual support** — Chinese and English voice commands
- **Sub-100ms response** — powered by Qualcomm NPU

## Quick Start

```bash
cd examples/automotive_assistant

# Run the built-in demo
sparx demo automotive

# Interactive session
sparx run
# > 打开空调，设置22度
# > Navigate to nearest charging station
# > 播放我的收藏歌单
```

## Supported Commands

| Category | Example Commands |
|----------|-----------------|
| Climate | "打开空调，设置22度，内循环" / "Turn on AC, set to 22°C" |
| Navigation | "导航到最近的充电站" / "Navigate to nearest charging station" |
| Media | "播放我最喜欢的歌单" / "Play my favorite playlist" |
| Phone | "给张三打电话" / "Call John" |
| Vehicle | "查看电量" / "Check battery level" |

## Architecture

```
User Voice Input
     │
     ▼
┌─────────────┐
│ Preprocessing│ ← UTF-8 normalization, parameter extraction
└──────┬──────┘
       ▼
┌─────────────┐
│ Skill Router │ ← Deterministic pattern matching (no model call!)
└──────┬──────┘
       ▼
┌─────────────┐
│ MCP Service  │ ← vehicle.climate / navigation / media
└──────┬──────┘
       ▼
┌─────────────┐
│   Response   │ ← Natural language confirmation
└─────────────┘

Total latency: <100ms (on Qualcomm NPU)
```

## Files

- `agent.yaml` — Agent configuration and skill list
- `skills/climate_control.yaml` — AC, temperature, circulation
- `skills/navigation.yaml` — Destination routing, POI search
- `skills/media.yaml` — Music playback control

## Customization

Add your own skills by creating a YAML file in `skills/`:

```yaml
name: my_skill
description: "What this skill does"
trigger:
  patterns:
    - "trigger phrase 1"
    - "trigger phrase 2"
handler:
  type: deterministic
  service: my.mcp.service
  response_template: "Done!"
```

---

<a name="中文"></a>

# 🚗 车载语音助手示例

一个完整的端侧车载语音助手，演示：

- **确定性技能路由** — 模式匹配，无需模型调用
- **MCP服务集成** — 车辆空调、导航、媒体控制
- **双语支持** — 中英文语音命令
- **亚百毫秒响应** — Qualcomm NPU驱动

## 快速开始

```bash
cd examples/automotive_assistant

# 运行内置演示
sparx demo automotive

# 交互式会话
sparx run
# > 打开空调，设置22度
# > Navigate to nearest charging station
# > 播放我的收藏歌单
```

## 支持的命令

| 类别 | 示例命令 |
|------|---------|
| 空调 | "打开空调，设置22度，内循环" |
| 导航 | "导航到最近的充电站" |
| 媒体 | "播放我最喜欢的歌单" |
| 电话 | "给张三打电话" |
| 车况 | "查看电量" |

## 自定义

在`skills/`目录创建YAML文件即可添加新技能。详见上方英文说明。
