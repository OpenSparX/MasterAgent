# 🏠 Smart Home Control Example

[English](#english) | [中文](#中文)

---

<a name="english"></a>

Control your smart home devices entirely on-device via MCP services.

## Quick Start

```bash
cd examples/smart_home
sparx run

# > Turn on living room lights
# > Set bedroom temperature to 23°C
# > Arm security system
```

## Features

- **Lights**: On/off, brightness, room selection
- **Thermostat**: Temperature, mode (heat/cool/auto)
- **Security**: Arm/disarm, camera check, alerts

## MCP Service Integration

This example shows how Sparx connects to IoT devices through the MCP protocol:

```
sparx run → Skill Match → MCP Call → IoT Device
                                    → Zigbee/Z-Wave/Wi-Fi
```

All processing happens on your edge device (e.g., a smart home hub running on Qualcomm QCS6490).

---

<a name="中文"></a>

# 🏠 智能家居控制示例

通过MCP服务在本地完全控制智能家居设备。

## 快速开始

```bash
cd examples/smart_home
sparx run

# > 打开客厅灯
# > 卧室温度设置23度
# > 启动安防系统
```

## 功能

- **灯光**：开关、亮度、房间选择
- **温控**：温度、模式（制热/制冷/自动）
- **安防**：布防/撤防、摄像头查看、告警
