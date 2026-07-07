# Surface Pro 5 - Howdy IR Camera Bridge

This repository contains an experimental set of tools, services, and configurations to enable the IR camera on the Microsoft Surface Pro 5 (Model 1796, Kaby Lake, IPU3) for use with [Howdy](https://github.com/boltgolt/howdy) (Face Unlock).

## ⚠️ STATUS & STABILITY WARNING ⚠️

**CRITICAL:** This project is highly experimental. It is NOT 100% stable.
- It is highly specific to the **Surface Pro 5** equipped with the **OV7251 IR sensor**.
- It bypasses standard kernel behaviors and might still possess edge-case race conditions.
- It desperately needs further tuning, testing, and refinement from the Linux-Surface community.
- Use at your own risk. This can and will break depending on your system configuration.

## Architecture

The Surface Pro 5 IR sensor outputs a proprietary 10-bit raw stream via the Intel IPU3 ISP, which Howdy's standard OpenCV/v4l2 backends cannot natively parse. Additionally, running a background translation daemon directly via systemd introduces massive Polkit/PAM recursion deadlocks during `sudo` authentication. 

We solved this via a stateless, on-demand pipeline:
1. **The C Bridge (`ir_bridge.c`)**: A highly optimized C program that reads the raw 10-bit IPU3 stream from `/dev/video12`, mathematicaly unpacks it to standard 8-bit GREY, and simultaneously rotates it 90-degrees (as the physical sensor is mounted sideways) without using CPU-heavy temporary buffers. It uses `poll()` to achieve near 0% CPU usage when idle.
2. **Delayed I2C Trigger**: The bridge manually toggles the OV7251's IR LED via I2C (`i2ctransfer`), but critically waits until the *first* valid frame is dequeued from the kernel driver to prevent `VIDIOC_STREAMON` initialization races from overwriting the LED register.
3. **Hardware Loopback**: The translated, rotated stream is fed into `v4l2loopback` (`/dev/video42`), which Howdy reads from.
4. **On-Demand Lifecycle**: Instead of using systemd or PAM hooks (which cause D-Bus deadlocks), the bridge is launched dynamically as a background subprocess directly inside Howdy's `VideoCapture` initialization logic. The camera only powers on the millisecond Howdy needs it, and safely tears down when Howdy exits.

## Installation

### 1. Prerequisites
You need standard build tools, v4l2loopback, and i2c-tools:
```bash
sudo dnf install gcc v4l2loopback i2c-tools
```

### 2. Configure v4l2loopback (Systemd Oneshot)
Copy the provided systemd service to initialize the loopback device on boot:
```bash
sudo cp surface_ir_bridge.service /etc/systemd/system/
sudo systemctl enable --now surface_ir_bridge.service
```

### 3. Compile and Install the Bridge
Compile the C bridge and place it in your local bin path:
```bash
gcc -o ir_bridge ir_bridge.c
sudo cp ir_bridge /usr/local/bin/ir_bridge
sudo chmod +x /usr/local/bin/ir_bridge
```

### 4. Patch Howdy's VideoCapture
*(Note: Injecting this into `video_capture.py` covers `sudo howdy test` and `sudo howdy add` universally, whereas modifying `compare.py` only covers authentication).*

Edit `/usr/lib/python3.X/site-packages/howdy/recorders/video_capture.py`:

**A. Add required imports at the top:**
```python
import subprocess
import time
```

**B. Inside the `__init__` method, right before `self._create_reader()`, add:**
```python
        # Start the IR bridge directly in the background before opening the device
        self.bridge_proc = subprocess.Popen(["/usr/local/bin/ir_bridge"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.4) # Wait for the loopback device to be populated
```

**C. Inside both the `__del__` and `release` methods, add cleanup logic:**
```python
            if hasattr(self, 'bridge_proc'):
                self.bridge_proc.terminate()
```

### 5. PAM Routing (KDE/SDDM Example)
To enable face unlock on the lock screen and GUI prompts, add `auth sufficient pam_howdy.so` to the absolute top of the following files in `/etc/pam.d/`:
- `/etc/pam.d/kde`
- `/etc/pam.d/kscreensaver`
- `/etc/pam.d/polkit-1`
- `/etc/pam.d/plasmalogin` (or `/etc/pam.d/sddm`)

*(Note: On Fedora, if the file exists only in `/usr/lib/pam.d/`, copy it to `/etc/pam.d/` first to prevent package updates from erasing your changes).*

### 6. Configure Howdy
Run `sudo howdy config` and ensure:
```ini
device_path = /dev/video42
dark_threshold = 60
```

## Community
Please test, fork, and report your findings! This pipeline is a proof-of-concept that demonstrates it *is* possible to get reliable, fast face unlock on the Surface Pro 5 in Linux.
