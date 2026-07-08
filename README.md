# Surface Pro 5 - Howdy IR Camera Bridge

This repository contains an experimental set of tools, services, and configurations to enable the IR camera on the Microsoft Surface Pro 5 (Model 1796, Kaby Lake, IPU3) for use with [Howdy](https://github.com/boltgolt/howdy) (Face Unlock).

## ⚠️ STATUS & STABILITY WARNING ⚠️

**CRITICAL:** This project is highly experimental. It is NOT 100% stable.
- It is highly specific to the **Surface Pro 5** equipped with the **OV7251 IR sensor**.
- It desperately needs further tuning, testing, and refinement from the Linux-Surface community.
- Use at your own risk. This can and will break depending on your system configuration.

## Architecture

The Surface Pro 5 IR sensor outputs a proprietary 10-bit raw stream via the Intel IPU3 ISP, which Howdy's standard OpenCV/v4l2 backends cannot natively parse. Additionally, running a background translation daemon directly via systemd introduces massive Polkit/PAM recursion deadlocks during `sudo` authentication. 

We solved this via a stateless, on-demand pipeline:
1. **The C Bridge (`ir_bridge.c`)**: A highly optimized C program that reads the raw 10-bit IPU3 stream, mathematically unpacks it to standard 8-bit GREY, and simultaneously rotates it 90-degrees (as the physical sensor is mounted sideways) without using CPU-heavy temporary buffers. It uses `poll()` to achieve near 0% CPU usage when idle.
2. **Direct I2C LED Trigger**: The bridge controls the OV7251's IR LED directly via I2C (`/dev/i2c-3`, address `0x60`, register `0x3005`), with return-value checking and bounded retries. No shell-out, no `setuid(0)`.
3. **Hardware Loopback**: The translated, rotated stream is fed into `v4l2loopback` (`/dev/video42`), which Howdy reads from.
4. **On-Demand Lifecycle**: Instead of using systemd or PAM hooks (which cause D-Bus deadlocks), the bridge is launched dynamically as a background subprocess directly inside Howdy's `VideoCapture` initialization logic. The camera only powers on the millisecond Howdy needs it, and safely tears down when Howdy exits. The bridge also has a hard 15-second self-timeout for defense in depth.

### IPU3 ip3y Pixel Format (`V4L2_PIX_FMT_IPU3_Y10`)

The raw sensor data uses Intel's packed 10-bit luminance format:
- **25 pixels packed into 32 bytes** as a contiguous little-endian 10-bit bitstream
- 6 groups of 4 pixels in 5 bytes (24 pixels, 30 bytes), plus 1 pixel in 2 bytes (6 bits padding)
- Per 5-byte group (bytes b0..b4):
  - Y'0 = `b0[7:0] | b1[1:0]<<8` → 10-bit, `>>2` for 8-bit
  - Y'1 = `b1[7:2] | b2[3:0]<<6` → 10-bit, `>>2` for 8-bit
  - Y'2 = `b2[7:4] | b3[5:0]<<4` → 10-bit, `>>2` for 8-bit
  - Y'3 = `b3[7:6] | b4[7:0]<<2` → 10-bit, `>>2` = `b4` (the two LSBs from b3 are lost in truncation)
- Reference: kernel `Documentation/userspace-api/media/v4l/pixfmt-yuv-luma.rst`

## Installation

### 1. Prerequisites
**CRITICAL: You MUST be running a kernel that supports the Surface IPU3 ISP and OV7251 sensor.** 
A standard distribution kernel will not work. You must be running the custom [linux-surface](https://github.com/linux-surface/linux-surface) kernel, or a custom patched kernel.

You also need standard build tools and v4l2loopback:
```bash
sudo dnf install gcc v4l2loopback
```

### 2. Configure v4l2loopback and Media Paths (Systemd Oneshot)
Because the Linux kernel randomly enumerates `/dev/media*` devices on boot, hardcoded media paths will fail after a restart. We provide a dynamic setup script that auto-detects the IPU3 sensor.

Copy the setup script and the systemd service to initialize the hardware on boot:
```bash
sudo cp setup_ipu3.sh /usr/local/bin/setup_ipu3.sh
sudo chmod +x /usr/local/bin/setup_ipu3.sh

sudo cp surface_ir_bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now surface_ir_bridge.service
```

### 3. Compile and Install the Bridge
Compile the C bridge and place it in your local bin path:
```bash
gcc -Wall -Wextra -O2 -o ir_bridge ir_bridge.c
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
        self.bridge_proc = None
        try:
            self.bridge_proc = subprocess.Popen(
                ["/usr/local/bin/ir_bridge"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.0)
        except Exception as e:
            print(f"Warning: could not start ir_bridge: {e}")
```

**C. Inside both the `__del__` and `release` methods, add cleanup:**
```python
            self._stop_bridge()
```

**D. Add the `_stop_bridge` method:**
```python
    def _stop_bridge(self):
        """Safely terminate the ir_bridge child process."""
        proc = getattr(self, 'bridge_proc', None)
        if proc is None:
            return
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except ProcessLookupError:
            pass
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=1)
        except Exception:
            pass
        self.bridge_proc = None
```

**E. In `_create_reader`, change `cv2.CAP_V4L` to `cv2.CAP_V4L2`:**
This eliminates the OpenCV backend-probing warning "can't be used to capture by name".

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
dark_threshold = 100
```

> **Why `dark_threshold = 100`?** The IR camera produces very dark images (max pixel value ~55 out of 255). Howdy's default threshold of 60 throws away perfectly good IR frames. The bridge itself filters truly dead/black frames before they reach Howdy.

### 7. Re-enroll Your Face
After installation or any pipeline changes, re-enroll your face:
```bash
sudo howdy add
```
Consider adding multiple samples at slightly different angles/distances for better reliability.

## Debugging

Run `ir_bridge` with the `-d` flag to dump sample frames for inspection:
```bash
sudo /usr/local/bin/ir_bridge -d /dev/video2
# Frames saved to /tmp/ir_bridge_debug/frame_NNNN_bNNN.raw
# View as: python3 -c "import numpy as np; d=np.fromfile('frame.raw',np.uint8).reshape(640,480); import cv2; cv2.imwrite('out.png',d)"
```

## Community
Please test, fork, and report your findings! This pipeline is a proof-of-concept that demonstrates it *is* possible to get reliable, fast face unlock on the Surface Pro 5 in Linux.
