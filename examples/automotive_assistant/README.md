# 🚗 Automotive Assistant Example

A fully on-device automotive voice assistant powered by OAK/OpenSparX.
Runs on Qualcomm SA8797P NPU — no cloud dependency.

## Features

- Voice command recognition (navigation, media, climate)
- Multi-turn conversation with context memory
- Vehicle CAN bus integration via skills
- Speculative execution for common driving patterns

## Quick Start

```bash
cd examples/automotive_assistant
sparx init        # Already configured via sparx.yml
sparx run         # Start the assistant
```

## Architecture

```
User Voice → STT → IntentDAG → Skill Dispatch → Vehicle API
                        ↓
              Formal Verification
              (safety constraints)
```

## Skills

| Skill | Description |
|-------|-------------|
| `navigation` | Route planning, POI search, ETA queries |
| `media_control` | Music playback, radio, podcast control |
| `climate` | HVAC temperature, fan speed, seat heating |
| `vehicle_status` | Battery, tire pressure, range estimation |

## Safety Guarantees

All plans are formally verified before execution:
- **No conflicting actuator commands** (CTL: AG ¬conflict)
- **Climate bounds respected** (16°C ≤ temp ≤ 30°C)
- **Navigation doesn't override active emergency routing**

---

# 🚗 车载智能助手示例

基于 OAK/OpenSparX 的全端侧车载语音助手，运行在高通 SA8797P NPU 上。

## 快速开始

```bash
cd examples/automotive_assistant
sparx run
```

支持语音指令：导航、媒体、空调、车辆状态查询。所有执行计划经过形式化验证确保安全。
