# WAL恢复机制

Sparx的WAL（Write-Ahead Logging）恢复系统确保Agent在崩溃后能够安全恢复，不会丢失数据或重复执行副作用。

## 核心概念

### 为什么需要WAL？

AI Agent与传统应用不同：它们执行**有副作用的操作**（支付、控制设备、发送消息）。崩溃后：

- **重试**可能导致重复执行（双重扣费）
- **忽略**可能导致操作丢失（钱去向不明）
- **WAL + Unknown**是唯一安全的选择

### 三种终态

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  COMMITTED  │     │   FAILED    │     │   UNKNOWN   │
│    已提交    │     │   已失败    │     │   未知      │
└─────────────┘     └─────────────┘     └─────────────┘
      ↑                    ↑                    ↑
   副作用确认           副作用否认          无法确认
   安全继续             安全回滚            需要对账
```

| 终态 | 含义 | 操作 |
|------|------|------|
| COMMITTED | 副作用已确认执行 | 继续后续流程 |
| FAILED | 副作用已确认未执行 | 安全回滚/重试 |
| UNKNOWN | 无法确认副作用状态 | 暂停，要求人工对账 |

## 工作原理

### 1. 写入WAL日志

每个有副作用的操作在执行前写入WAL：

```
[WAL Record]
├─ invocation_id: a3f1c7e2
├─ service: payment.charge
├─ parameters: {amount: 49.99, currency: "CNY"}
├─ idempotency_key: pay_20260810_001
├─ timestamp: 2026-08-10T10:00:00Z
└─ state: PENDING
```

### 2. 执行操作

调用MCP服务执行实际操作。

### 3. 确认结果

```
成功 → state: COMMITTED
失败 → state: FAILED
崩溃/超时 → state: UNKNOWN（自动）
```

### 4. 恢复流程

重启后，WAL扫描所有PENDING记录：

```bash
# 恢复时的处理逻辑
for record in WAL.pending():
    if record.has_idempotency_response():
        # 服务端有幂等记录，可以安全查询
        result = service.query(record.idempotency_key)
        record.resolve(result)
    else:
        # 无法确认，进入UNKNOWN
        record.mark_unknown()
        notify_operator(record)
```

## Unknown终态详解

### 为什么Unknown是创新？

传统系统只有两种终态：成功或失败。但现实中存在第三种情况：**我们不知道**。

**场景示例**：
1. Agent发起支付请求
2. 请求已发送到网络
3. 此刻设备断电
4. 我们不知道支付是否成功

**错误做法**：
- 假设失败 → 重试 → 可能双重扣费
- 假设成功 → 继续 → 可能钱丢了
- 超时重试 → 同样危险

**Sparx做法**：
- 标记为UNKNOWN
- 记录幂等键
- 要求显式对账
- 提供对账工具

### 对账命令

```bash
# 查看所有UNKNOWN状态的操作
sparx reconcile --list

# 输出：
# ⚠️  2 operations in UNKNOWN state:
#   1. payment.charge  key=pay_20260810_001  amount=49.99 CNY
#   2. vehicle.unlock  key=unlock_20260810_003
#
# Use 'sparx reconcile --resolve <id> <committed|failed>' to resolve

# 手动对账（确认后）
sparx reconcile --resolve 1 committed
sparx reconcile --resolve 2 failed
```

## WAL文件结构

```
.sparx/
└── wal.log          # Write-Ahead Log
    ├── segment_001  # 已归档段
    ├── segment_002  # 已归档段
    └── active       # 当前活跃段
```

每个WAL段：
- 最大4MB（可配置）
- COMMITTED/FAILED记录在checkpoint后清理
- UNKNOWN记录保留直到显式解决

## 配置

在`agent.yaml`中配置WAL行为：

```yaml
recovery:
  wal:
    enabled: true
    segment_size_mb: 4
    checkpoint_interval_s: 60
    max_unknown_age_h: 168    # 7天后提醒
  unknown:
    auto_notify: true
    notify_channel: "operator"
    escalation_after_h: 24
```

## 与其他系统的对比

| 特性 | Sparx WAL | Kafka事务 | 数据库WAL |
|------|-----------|-----------|-----------|
| 粒度 | Agent操作级 | 消息级 | 行级 |
| Unknown终态 | ✅ | ❌ | ❌ |
| 幂等键追踪 | ✅ | 需手动 | ❌ |
| 边缘设备优化 | ✅ | ❌ | ❌ |
| 离线恢复 | ✅ | 需要Broker | ✅ |

## 参考

- [系统概述](01_系统概述.md) — 整体架构
- [数据日志与异常](08_数据日志与异常.md) — 日志和审计
- [接口与执行流程](09_接口与执行流程.md) — 完整执行链
