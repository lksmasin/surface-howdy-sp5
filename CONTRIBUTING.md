# Contributing to Surface-Howdy-SP5

Thank you for your interest in improving the Surface Pro 5 IR bridge for Howdy!

This project is highly experimental and directly interacts with the kernel, hardware peripherals, and the PAM (Pluggable Authentication Modules) stack. Because it runs with elevated privileges to authenticate users, we need to be extremely careful with contributions.

## Filing a Good Bug Report
If you run into issues, please file a bug report. Since this project is highly hardware-specific, your report **must** include:
- Your exact Surface model and kernel version.
- Whether you are running the official `linux-surface` kernel or a custom build.
- The output of `media-ctl -p` (to confirm your sensor is indeed the OV7251 or if it's another variant).
- The version of Howdy you are using.
- Relevant `ir_bridge` stderr output (run it manually or check `journalctl`).

## Submitting a Pull Request
Before submitting a PR, please ensure you have read the following guidelines:

### 1. Test on Real Hardware with a Fallback Login
**CRITICAL:** Because this bridge is integrated with PAM (e.g., your lock screen and `sudo`), a bug could lock you out of your system. 
- Always ensure you have a fallback login method available (e.g., standard password auth enabled in `/etc/pam.d/`, or an active root shell in another TTY) before testing your changes.
- Do not submit PRs with "it compiles" as the only validation. You must verify that face detection, LED triggering, and system stability remain intact on real hardware.

### 2. C Code Expectations (`ir_bridge.c`)
- **No shell-outs:** Do not use `system()`, `popen()`, or similar functions to perform privileged operations. Use direct syscalls and `ioctl` (like the I2C implementation).
- **Check return values:** Always verify the return values of syscalls (e.g., `open`, `write`, `ioctl`). Failing silently can lead to orphaned processes or security flaws.
- **Maintain bounds and timeouts:** The bridge features a hard self-timeout (currently 15 seconds) to prevent infinite loops in the background if Howdy crashes. Do not remove or bypass this mechanism.

Please use the provided Issue and Pull Request templates when submitting.
