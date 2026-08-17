# 🌐 IoT Edge Agent Example

An on-device agent for IoT edge gateways that processes sensor data,
detects anomalies, and triggers automations — all locally without cloud.

## Features

- Real-time sensor data processing (temperature, humidity, motion, power)
- Anomaly detection with local ML inference
- Multi-device mesh coordination via Agent Mesh Protocol
- Rule-based automation with formal verification

## Quick Start

```bash
cd examples/iot_edge
sparx init
sparx run
```

## Architecture

```
Sensors → MQTT Ingestion → Agent (NPU inference) → Actuators
                                    ↓
                          Mesh sync with other edge nodes
```

## Use Cases

- **Smart Factory**: Machine health monitoring, predictive maintenance
- **Agriculture**: Soil moisture, irrigation control, frost alerts
- **Energy**: Solar panel optimization, battery management
- **Security**: Perimeter monitoring, anomaly detection

## Skills

| Skill | Description |
|-------|-------------|
| `sensor_monitor` | Ingest and analyze sensor streams |
| `anomaly_detect` | Local ML-based anomaly detection |
| `automation` | Rule engine for sensor→actuator mappings |
| `mesh_sync` | Coordinate with neighboring edge nodes |

---

# 🌐 IoT 边缘智能体示例

边缘网关上的全端侧 AI Agent，处理传感器数据、异常检测、自动化控制。
支持通过 Agent Mesh Protocol 与其他边缘节点协同。
