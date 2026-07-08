---
name: Bug report
about: Create a report to help us improve this project.
title: "[BUG]"
labels: ''
assignees: lksmasin

---

**Describe the bug**
A clear and concise description of what the bug is (e.g., C bridge crashes, black screen, Howdy reports a timeout error).

**Hardware and Software Context**
To help us resolve the issue, PLEASE fill out the following information:

- **Device Model:** (e.g., Surface Pro 5, i5, 8GB)
- **Linux Distribution and Version:** (e.g., Fedora 44, Ubuntu 24.04)
- **Kernel Version (output of `uname -r`):** - **Are you running the `linux-surface` kernel?** [Yes/No]

**Diagnostics (Critical for troubleshooting)**
Please paste the terminal outputs below:

1. Camera check output:
```bash
# Paste the output of: sudo media-ctl -p | grep ov7251
```

2. System service status:
```bash
# Paste the output of: systemctl status surface_ir_bridge
```

3. (Optional) Howdy test:
```bash
# Paste the output of: sudo howdy test
```

**To Reproduce**
Steps to reproduce the behavior:
1. I did...
2. I ran the command...
3. I see this error...

**Additional context**
Add any other context about the problem here (e.g., desktop environment used, whether the issue occurred after waking from sleep, etc.).
