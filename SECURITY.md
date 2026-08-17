# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 2.1.x   | ✅ Active support  |
| 2.0.x   | ⚠️ Security fixes only |
| < 2.0   | ❌ End of life     |

## Reporting a Vulnerability

If you discover a security vulnerability, please report it responsibly:

**Email**: security@opensparx.dev

**Do NOT** open a public GitHub issue for security vulnerabilities.

### What to Include

- Description of the vulnerability
- Steps to reproduce
- Potential impact assessment
- Suggested fix (if any)

### Response Timeline

- **Acknowledgment**: Within 48 hours
- **Assessment**: Within 7 days
- **Fix**: Within 30 days for critical, 90 days for non-critical

## Security Model

### Agent Isolation

OAK uses capability-based access control. Each agent operates in a sandbox:

- No ambient authority (deny by default)
- Explicit capability tokens for each resource
- Time-bounded permissions with automatic expiry
- Full audit logging of all access decisions

### On-Device Privacy

- All inference runs locally — no data leaves the device
- No telemetry or usage reporting
- Memory entries have configurable TTL with automatic expiry
- `sparx forget` command for explicit data deletion

### Qualcomm Licensed Components

⚠️ **CRITICAL**: The following directories contain Qualcomm AI Stack Licensed code:

- `genai_lib/`
- `qnn_model_prepare_*.py`
- `utilities/`

**71 of 99 Python files** are marked "Confidential and Proprietary — Qualcomm
Technologies, Inc."

These files **MUST NOT**:
- Be included in any public repository
- Be redistributed standalone
- Be included in pull requests
- Be referenced in public documentation

**Before each release**, verify that no Qualcomm-headered file has migrated
into the open-source tree:

```bash
# Run this check before any public release
grep -rl "Confidential and Proprietary" --include="*.py" . | \
  grep -v "genai_lib/" | grep -v "utilities/" | grep -v "qnn_model_prepare"
# Output should be empty
```

### Supply Chain Security

- All dependencies are vendored in `third_party/`
- No runtime network dependencies
- Reproducible builds via pinned CMake versions
- Binary releases include SHA-256 checksums

## Threat Model

OAK is designed for on-device deployment. The primary threats are:

1. **Prompt injection** — Mitigated by formal plan verification (CTL model checking)
2. **Tool misuse** — Mitigated by capability-based access control
3. **Resource exhaustion** — Mitigated by per-agent resource quotas
4. **Privilege escalation** — Mitigated by deny-by-default security policy
5. **Data exfiltration** — Mitigated by network permission requirements

---

# 安全策略

## 报告漏洞

发现安全漏洞请发送邮件至 security@opensparx.dev，请勿公开提交 Issue。

## 高通授权代码

`genai_lib/`、`qnn_model_prepare_*.py`、`utilities/` 下的文件为高通机密授权代码，
严禁出现在任何公开仓库或 Pull Request 中。每次发版前必须验证无泄露。
