# 🏠 Smart Home Agent Example

A privacy-first smart home assistant running entirely on a local device.
No voice data leaves your home. No cloud subscriptions needed.

## Features

- Natural language home control (lights, locks, appliances)
- Scene management (movie night, good morning, away mode)
- Energy monitoring and optimization
- Multi-room awareness via Agent Mesh
- Guest access with capability-based permissions

## Quick Start

```bash
cd examples/smart_home
sparx init
sparx run
```

## Architecture

```
Voice/Text → Agent (local LLM) → Home API (Zigbee/Z-Wave/WiFi)
                    ↓
         Access Control (per-user permissions)
         Memory (user preferences, routines)
```

## Skills

| Skill | Description |
|-------|-------------|
| `lighting` | Room/zone lighting control, color, brightness |
| `security` | Locks, cameras, alarm system |
| `scenes` | Multi-device scene activation |
| `energy` | Power monitoring, solar, battery management |

## Privacy Model

- All processing on-device (no cloud STT/TTS)
- Access Control enforces per-user capability tokens
- Guest mode: limited to specific rooms and devices
- No data retention beyond configurable TTL

---

# 🏠 智能家居助手示例

完全本地运行的智能家居助手，所有语音数据留在家中。
支持能力权限模型，访客模式仅限特定房间和设备。
