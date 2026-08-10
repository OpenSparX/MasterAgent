# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 2.1.x   | ✅ Current release |
| 2.0.x   | ✅ Security fixes  |
| < 2.0   | ❌ End of life     |

## Reporting a Vulnerability

**Do NOT open a public issue for security vulnerabilities.**

Please report security vulnerabilities via email:

📧 **security@openschbrid.com**

Include:
- Description of the vulnerability
- Steps to reproduce
- Impact assessment
- Suggested fix (if any)

## Response Timeline

- **Acknowledgment**: Within 48 hours
- **Initial assessment**: Within 5 business days
- **Fix release**: Within 30 days for critical issues

## Scope

Security issues we care about:

- Remote code execution in the Sparx CLI or daemon
- WAL corruption that could lead to incorrect agent state
- Privilege escalation on deployed devices
- MCP service injection or unauthorized capability invocation
- Memory safety issues (buffer overflow, use-after-free)
- Cryptographic weaknesses in checksum verification

## Out of Scope

- Vulnerabilities in third-party dependencies (report upstream)
- Denial of service via excessive resource consumption (unless trivially exploitable)
- Issues requiring physical access to a deployed device

## Recognition

We appreciate responsible disclosure. Contributors who report valid security
issues will be acknowledged in release notes (unless they prefer anonymity).

## Qualcomm QNN SDK

Security issues related to the Qualcomm QNN SDK should be reported to Qualcomm
directly, as the SDK is covered by their own security policy and NDA terms.
