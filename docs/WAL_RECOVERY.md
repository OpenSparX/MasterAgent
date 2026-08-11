# WAL Recovery

Sparx uses Write-Ahead Logging to ensure agents recover safely from crashes without losing data or duplicating side effects.

## The Problem

AI agents execute operations with real-world consequences (payments, device control, messages). After a crash:

- **Retry** risks duplication (double-charging)
- **Ignore** risks data loss (money disappears)
- **WAL + Unknown** is the only safe choice

## Three Terminal States

| State | Meaning | Action |
|-------|---------|--------|
| COMMITTED | Side effect confirmed executed | Continue |
| FAILED | Side effect confirmed not executed | Safe to retry |
| UNKNOWN | Cannot confirm either way | Halt, require reconciliation |

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  COMMITTED  │     │   FAILED    │     │   UNKNOWN   │
└─────────────┘     └─────────────┘     └─────────────┘
      ↑                    ↑                    ↑
  Effect confirmed    Effect denied     Cannot determine
  Safe to continue    Safe to rollback   Requires reconcile
```

## How It Works

1. **Write WAL record** before executing any side effect
2. **Execute** the MCP service call
3. **Confirm result** → update WAL state
4. **On crash** → scan PENDING records at restart:
   - If idempotency key exists → query service for actual result
   - If unknown → mark UNKNOWN, notify operator

## The Unknown State (Sparx Innovation)

Traditional systems only have success/failure. Reality has a third case: **we don't know.**

**Scenario**: Agent sends a payment request, then the device loses power. Did the payment go through?

| Framework | Approach | Risk |
|-----------|----------|------|
| LangChain | Retry | Double-charge |
| AutoGPT | Ignore | Lost money |
| **Sparx** | **UNKNOWN → reconcile** | **None** |

## Reconciliation

```bash
# List operations in UNKNOWN state
sparx reconcile --list

# Resolve after manual verification
sparx reconcile --resolve <id> committed
sparx reconcile --resolve <id> failed
```

## Configuration

```yaml
recovery:
  wal:
    enabled: true
    segment_size_mb: 4
    checkpoint_interval_s: 60
  unknown:
    auto_notify: true
    escalation_after_h: 24
```

## WAL File Layout

```
.sparx/
└── wal.log
    ├── segment_001   # archived
    ├── segment_002   # archived
    └── active        # current
```

- COMMITTED/FAILED records are cleaned at checkpoint
- UNKNOWN records persist until explicitly resolved

## See Also

- [系统概述 / System Overview](SYSTEM_OVERVIEW.md)
- [数据日志与异常](08_数据日志与异常.md)
- [WAL恢复机制（中文详细版）](WAL_RECOVERY_zh-CN.md)
