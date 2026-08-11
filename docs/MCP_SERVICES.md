# MCP Services

MCP (Model Context Protocol) is how Sparx connects agents to external capabilities — vehicle control, navigation, smart home devices, and IoT sensors.

## Overview

```
Agent
  │
  ▼
┌─────────────────┐
│  MCP Registry   │ ← Service catalogue
└────────┬────────┘
         │
    ┌────┼────┐
    ▼    ▼    ▼
┌──────┐┌──────┐┌──────┐
│Vehicle││ Home ││ IoT  │ ← Atomic services
└──────┘└──────┘└──────┘
```

## Core Concepts

Each MCP service is an **atomic operation** with:
- Defined input/output schema
- Idempotency guarantee (via idempotency keys)
- Timeout and retry policy
- WAL integration for side-effect tracking

## Adding a Custom Service

### 1. Define the schema

```yaml
# services/home_lights.yaml
name: home.lights
version: "1.0"
methods:
  - name: set_state
    input:
      room: {type: string, required: true}
      state: {type: enum, values: [on, off]}
      brightness: {type: int, range: [0, 100], default: 100}
    output:
      success: {type: bool}
    side_effect: true
    idempotent: true
    timeout_ms: 3000
```

### 2. Register in agent.yaml

```yaml
mcp:
  services:
    - path: services/home_lights.yaml
```

### 3. Reference from a skill

```yaml
# skills/lights_control.yaml
handler:
  type: deterministic
  service: home.lights
  method: set_state
  parameter_mapping:
    room: "{entities.room}"
    state: "{entities.action}"
```

## Built-in Services

| Service | Description | Domain |
|---------|-------------|--------|
| `vehicle.climate` | AC / heating | Automotive |
| `vehicle.media` | Playback control | Automotive |
| `vehicle.navigation` | Destination set | Automotive |
| `home.lights` | Light control | Smart home |
| `home.thermostat` | HVAC control | Smart home |
| `home.security` | Alarm system | Smart home |
| `iot.sensor` | Sensor read | IoT edge |
| `iot.alert` | Alert trigger | IoT edge |
| `iot.data_sync` | Batch upload | IoT edge |

## Error Handling

```yaml
error_codes:
  TIMEOUT: Service call exceeded deadline
  UNAVAILABLE: Service unreachable
  INVALID_PARAMS: Schema validation failed
  DEVICE_ERROR: Underlying device failure
  UNKNOWN: Cannot determine result → triggers WAL Unknown flow
```

## Priority Scheduling

| Priority | Use Case | Preemption |
|----------|----------|------------|
| P0 | Safety-critical (emergency brake, smoke alarm) | Immediate |
| P1 | User interaction (climate, navigation) | Queue head |
| P2 | Background (data sync, logging) | Can be preempted |

## See Also

- [原子服务与MCP](06_原子服务与MCP.md) — Technical deep-dive
- [WAL Recovery](WAL_RECOVERY.md) — Crash safety
- [MCP服务集成（中文详细版）](MCP_SERVICES_zh-CN.md)
