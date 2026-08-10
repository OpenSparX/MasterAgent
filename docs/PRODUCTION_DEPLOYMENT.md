# Production Deployment Guide

This guide covers best practices for deploying Sparx agents in production environments — automotive, IoT, robotics, and edge computing scenarios.

---

## 1. Pre-Deployment Validation

### Hardware Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| **NPU** | Qualcomm SA8155 | SA8650 (Gen 3) |
| **RAM** | 2 GB | 4 GB+ |
| **Storage** | 512 MB free | 2 GB free |
| **OS** | Linux 4.14+ | QNX 7.1+ / Linux 5.10+ |

**Verify NPU availability:**
```bash
sparx doctor --check-npu
# Expected output:
# ✓ QNN runtime found
# ✓ HTP backend available
# ✓ Test inference: 87ms
```

### Model Validation

Test on **real user inputs from your domain**, not generic benchmarks:

```bash
# Collect 100 real commands from your application
cat real_commands.txt | sparx test my-agent --batch

# Accuracy must be ≥95% for safety-critical systems
# Latency p95 must be <200ms
```

---

## 2. Security Hardening

### File Permissions

Sparx agents run with user privileges. Lock down sensitive files:

```bash
# Agent config (contains MCP credentials)
chmod 600 /opt/sparx/agent.yaml

# WAL directory (crash recovery state)
chmod 700 /opt/sparx/wal/

# Model files (prevent tampering)
chmod 444 /opt/sparx/models/*.bin
chown root:root /opt/sparx/models/*.bin
```

### Network Isolation (If MCP is Remote)

```yaml
# agent.yaml
mcp_services:
  - name: vehicle.climate
    url: http://127.0.0.1:8080  # Localhost only, not 0.0.0.0
    auth:
      type: token
      secret_file: /etc/sparx/mcp_token  # Not inline in YAML
```

**Best practice:** Use Unix domain sockets instead of TCP:
```yaml
mcp_services:
  - name: vehicle.climate
    socket: /var/run/sparx/climate.sock
```

### Input Validation

Even on-device, validate user input:

```yaml
# agent.yaml
security:
  max_input_length: 256      # Reject excessively long inputs
  rate_limit_per_minute: 60  # Prevent DoS from malicious users
  sanitize_mcp_output: true  # Strip control chars from MCP responses
```

---

## 3. Reliability Configuration

### WAL (Write-Ahead Logging)

**Critical for safety:** If your agent controls physical systems (brakes, locks, HVAC), enable WAL:

```yaml
reliability: D1              # ACID-level durability
wal:
  directory: /var/lib/sparx/wal
  fsync_policy: always       # Survive power loss
  checkpoint_interval: 1     # Every command
```

**Test crash recovery:**
```bash
# Simulate crash mid-execution
sparx run my-agent &
PID=$!
sleep 2
kill -9 $PID  # Hard kill

# Restart — should resume from last checkpoint
sparx run my-agent
# Check logs for "recovered from WAL"
```

### Unknown Terminal State Detection

Unique to Sparx: detects when MCP calls may have partially executed.

```yaml
# agent.yaml
reliability: D1
unknown_state_policy: alert_and_halt  # Stop execution, require manual intervention
# Options: alert_and_halt, retry_idempotent, log_and_continue
```

**Example scenario:** Agent sends "unlock door" to MCP, then network fails before receiving confirmation. Unknown if door is locked or unlocked — halt and alert user.

---

## 4. Monitoring & Observability

### Health Check Endpoint

Run Sparx in daemon mode with HTTP health checks:

```bash
sparx daemon --health-port 9090
```

```bash
# Kubernetes liveness probe
curl http://localhost:9090/health
# {"status":"ok","latency_p95_ms":87,"uptime_seconds":3600}
```

### Structured Logging

```yaml
# agent.yaml
logging:
  level: info                # error, warn, info, debug
  format: json               # JSON for log aggregation (Splunk, ELK)
  output: /var/log/sparx/agent.log
  rotate_size_mb: 100
```

**Key metrics to monitor:**
- `latency_ms` (p50, p95, p99)
- `mcp_call_duration_ms` (per service)
- `error_rate` (per hour)
- `unknown_state_count` (should be zero in normal operation)
- `wal_recovery_count` (non-zero = system instability)

### Alerting

```yaml
# agent.yaml
telemetry:
  alert:
    - condition: latency_p95 > 200ms
      action: log_and_notify
      destination: ops@example.com
    - condition: unknown_state_detected
      action: halt_and_page
      destination: oncall@example.com
```

---

## 5. Deployment Patterns

### Automotive (In-Vehicle)

**Architecture:** Sparx runs on the IVI (In-Vehicle Infotainment) SoC, communicates with vehicle ECUs via CAN/Ethernet.

```
User Voice → Sparx Agent (SA8650) → MCP Services → CAN Gateway → ECUs
```

**Config:**
```yaml
model:
  backend: qnn_htp
  context_length: 512        # Voice commands are short
reliability: D1              # Safety-critical
power:
  mode: always_on            # Vehicle is running
```

**Deployment:**
1. Install via `.deb` package (not `curl | sh` on production ECUs)
2. Systemd service with auto-restart
3. Mount `/var/lib/sparx` on persistent storage (not tmpfs)

```ini
# /etc/systemd/system/sparx-agent.service
[Unit]
Description=Sparx Automotive Agent
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/sparx daemon /etc/sparx/automotive.yaml
Restart=always
RestartSec=5
User=sparx
Group=vehicle

[Install]
WantedBy=multi-user.target
```

### IoT Edge (Battery-Powered)

**Architecture:** Sparx on gateway device, coordinates sensors via MQTT/Zigbee.

```yaml
power:
  mode: low_power
  wake_on: [sensor_threshold, scheduled]
  sleep_after_idle_ms: 10000
model:
  backend: qnn_dsp
  context_length: 1024
```

**Battery life optimization:**
- Use `qwen3-2b` (smallest model)
- Enable `deterministic_first` routing (skip LLM when possible)
- Set aggressive sleep policy

**Expected battery life:** 7-14 days on 5000mAh with 10 agent invocations/day.

### Smart Home (Always-On)

```yaml
model:
  backend: qnn_htp
  context_length: 2048
reliability: D2              # Less critical than automotive
power:
  mode: always_on
```

**Integration:** HomeAssistant, Home Bridge, or custom MQTT broker.

---

## 6. Update Strategy

### OTA (Over-The-Air) Updates

**Two-stage update for safety:**

```bash
# Stage 1: Download and validate new version
sparx update --stage v2.1.7
# Downloads to /tmp/sparx-staged/, validates checksum

# Stage 2: Apply on next reboot (automotive) or immediately (IoT)
sparx update --apply
```

**Rollback on failure:**
```bash
# If new version crashes on startup, auto-rollback after 3 failed attempts
sparx daemon --auto-rollback-threshold 3
```

### Model Updates

Models are large (100-500 MB). Update strategy depends on bandwidth:

**Option 1: Incremental delta updates (recommended)**
```bash
# Only download changed layers
sparx model update qwen3-4b --delta
# Saves 80% bandwidth vs full download
```

**Option 2: Staged rollout**
```bash
# Update 10% of fleet first, monitor for 24h, then 100%
sparx model update --canary-percent 10
```

---

## 7. Disaster Recovery

### Backup Critical State

```bash
# Automated daily backup
0 2 * * * tar -czf /backup/sparx-$(date +\%Y\%m\%d).tar.gz \
  /var/lib/sparx/wal \
  /etc/sparx/agent.yaml \
  /var/log/sparx/
```

### Recovery from Corrupted WAL

```bash
# If WAL is corrupted (disk failure, power loss during write)
sparx recover --wal /var/lib/sparx/wal --output /tmp/recovered_state.json

# Inspect recovered state
cat /tmp/recovered_state.json

# Rebuild WAL from last known good state
sparx init-wal --from /tmp/recovered_state.json
```

### Factory Reset

```bash
# Remove all state, restore to fresh install
sparx reset --keep-config
# Keeps agent.yaml and skills/, removes WAL and logs
```

---

## 8. Compliance & Certification

### Automotive (ISO 26262)

Sparx is designed for ASIL-B compliance:

- **WAL durability**: Survives power loss mid-transaction
- **Unknown terminal state**: Detects partial execution
- **Deterministic routing**: Bypasses LLM for safety-critical commands

**Certification checklist:**
- [ ] FMEA (Failure Mode and Effects Analysis) completed
- [ ] WAL recovery tested under all power-loss scenarios
- [ ] Latency verified <200ms for safety-critical commands
- [ ] Input validation rejects malformed commands
- [ ] Audit logs retained for 90 days

### Medical Devices (IEC 62304)

If deploying in healthcare (e.g., patient monitoring):

- [ ] Enable full audit logging (`logging.level: debug`)
- [ ] Cryptographically sign agent.yaml and models
- [ ] Use hardware-backed key storage (TPM/TEE)
- [ ] Implement user authentication (not just device auth)

---

## 9. Performance Regression Testing

### Continuous Benchmarking

```yaml
# .github/workflows/performance-test.yml
- name: Benchmark on target hardware
  run: |
    sparx profile examples/automotive_assistant --iterations 1000
    # Fail if p95 > 150ms
    sparx profile --assert-p95 150
```

### A/B Testing New Models

```bash
# Run two agent versions side-by-side, split traffic 50/50
sparx ab-test --control v2.1.6 --variant v2.1.7 --traffic-split 50

# Metrics after 1000 requests:
# Control: p95=87ms, accuracy=96.2%
# Variant: p95=92ms, accuracy=97.1%  ← 5ms slower but more accurate
```

---

## 10. Production Checklist

### Pre-Launch
- [ ] Hardware verified with `sparx doctor --check-npu`
- [ ] Tested on ≥100 real user inputs (≥95% accuracy)
- [ ] Latency p95 < 200ms on target hardware
- [ ] WAL crash recovery tested (kill -9 test)
- [ ] Security: file permissions locked down (chmod 600)
- [ ] MCP services use Unix sockets or localhost only
- [ ] Health check endpoint responding (`/health`)
- [ ] Logs forwarded to aggregator (Splunk/ELK)
- [ ] Alerting configured for latency/error spikes
- [ ] OTA update tested (stage → apply → rollback)

### Post-Launch Monitoring (First 7 Days)
- [ ] Latency p95 remains <200ms under real traffic
- [ ] Zero `unknown_state_detected` alerts
- [ ] Memory usage stable (no leaks)
- [ ] CPU usage <40% average (headroom for spikes)
- [ ] No WAL recovery events (indicates system stability)
- [ ] User-reported issues <1% of total interactions

---

## Common Production Issues

| Symptom | Root Cause | Fix |
|---------|-----------|------|
| Agent won't start after reboot | WAL corruption | `sparx recover --wal` then `sparx init-wal` |
| Latency degrades over days | Memory leak in MCP service | Restart MCP services nightly via cron |
| Random "unknown state" alerts | MCP timeout too short | Increase `mcp_timeout_ms` in agent.yaml |
| High CPU usage | Model not quantized | Verify INT8 quantization: `file model.bin` should show "data" not "ELF" |
| Permission denied errors | SELinux/AppArmor blocking | `audit2allow` or add `/opt/sparx/**` to policy |

---

## Support & Escalation

- **Documentation**: https://github.com/OpenSparX/MasterAgent/tree/main/docs
- **Community**: https://github.com/OpenSparX/MasterAgent/discussions
- **Bug reports**: https://github.com/OpenSparX/MasterAgent/issues
- **Security vulnerabilities**: security@opensparx.org (PGP key in SECURITY.md)

For production deployments, consider:
- **Enterprise support**: On-call assistance, SLA guarantees
- **Custom model training**: Domain-specific fine-tuning for your use case
- **Certification assistance**: ISO 26262, IEC 62304, FCC/CE compliance