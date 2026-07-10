# Security Policy

## Supported Versions

Currently, only the **master** branch is actively supported and evaluated for security updates. Given the maturity of the project, older revisions or detached forks are not guaranteed to receive backported security patches.

| Version | Supported |
| ------- | --------- |
| `master`| Yes       |
| Older   | No        |

*(Note: If you are using an older build or a pre-packaged Copr build from third parties, please update to the latest `master`. Early versions of this bridge contained severe privilege-escalation bugs involving world-writable `/tmp` paths and `setuid(0)` shell-outs, which have since been fixed.)*

## Reporting a Vulnerability

Because this project provides an authentication bridge that hooks directly into PAM (Pluggable Authentication Modules), it runs privileged code during the login process. **We take security reports very seriously.**

If you discover a vulnerability, **please do not open a public issue.** Instead, please report it privately:
1. Use GitHub's **Private Vulnerability Reporting** feature on this repository (go to Security -> Advisories -> "Report a vulnerability").
2. Or, if that is unavailable, open a draft Security Advisory.

### What to include
Please include:
- A description of the vulnerability and its impact.
- Steps to reproduce the issue.
- Whether it requires local or remote access.
- If possible, a suggestion for a fix or mitigation.

We will acknowledge your report promptly, investigate the issue, and push a patch to the `master` branch. Once the fix is merged and public, we will publish the advisory.
