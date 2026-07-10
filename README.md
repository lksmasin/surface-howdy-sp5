# Surface Pro 5 - Howdy IR Camera Bridge

This repository contains an experimental set of tools, services, and configurations to enable the IR camera on the Microsoft Surface Pro 5 (Model 1796, Kaby Lake, IPU3) for use with [Howdy](https://github.com/boltgolt/howdy) (Face Unlock).

## STATUS & STABILITY WARNING

**CRITICAL:** This project is highly experimental. It is NOT 100% stable.
- It is highly specific to the **Surface Pro 5** equipped with the **OV7251 IR sensor**.
- It desperately needs further tuning, testing, and refinement from the Linux-Surface community.
- **Use at your own risk.** This modifies PAM authentication paths; bugs can and will break your ability to log in depending on your system configuration.
- **Security Notice:** Early versions of this bridge contained severe privilege-escalation bugs involving world-writable `/tmp` paths and `setuid(0)` shell-outs. If you downloaded a pre-packaged build or cloned this repository prior to July 2026, please update to the latest `master` immediately.

## Architecture Overview

The Surface Pro 5 IR sensor outputs a proprietary 10-bit raw stream via the Intel IPU3 ISP, which Howdy's standard OpenCV/v4l2 backends cannot natively parse. 

We solved this via a stateless, on-demand pipeline:
1. **The C Bridge (`ir_bridge.c`)**: A highly optimized C program that reads the raw 10-bit IPU3 stream, mathematically unpacks it to standard 8-bit GREY, and simultaneously rotates it 90-degrees (as the physical sensor is mounted sideways).
2. **Hardware Loopback**: The translated, rotated stream is fed into `v4l2loopback` (`/dev/video42`), which Howdy reads from.
3. **On-Demand Lifecycle**: The bridge is launched dynamically as a background subprocess directly inside Howdy's initialization logic. The camera only powers on the millisecond Howdy needs it, and safely tears down when Howdy exits.

## Quick Start & Documentation

Because the installation process involves compiling C code, setting up systemd services, writing udev rules, and patching Python scripts, we have moved the detailed documentation to the Wiki.

**Please see the [Wiki](https://github.com/lksmasin/surface-howdy-sp5/wiki) for full instructions:**

- **[Installation Guide](https://github.com/lksmasin/surface-howdy-sp5/wiki/Installation)**: Step-by-step instructions on prerequisites, compiling, systemd configuration, and patching Howdy.
- **[Troubleshooting](https://github.com/lksmasin/surface-howdy-sp5/wiki/Troubleshooting)**: Solutions for common issues like dark frames, LED failing to trigger, or PAM deadlocks.
- **[Hardware & Format Notes](https://github.com/lksmasin/surface-howdy-sp5/wiki/Hardware-and-Format-Notes)**: Technical deep-dive into the IPU3 `ip3y` pixel format, I2C LED triggers, and OV7251 sensor characteristics.

## Community

Please test, fork, and report your findings! This pipeline is a proof-of-concept that demonstrates it *is* possible to get reliable, fast face unlock on the Surface Pro 5 in Linux. 

Before contributing, please review our [Contributing Guidelines](CONTRIBUTING.md) and [Code of Conduct](CODE_OF_CONDUCT.md). Ensure that any security issues are reported via our [Security Policy](SECURITY.md).
