#!/bin/bash
# setup_ipu3.sh — Dynamically configure the IPU3 media graph for the OV7251 sensor.
#
# Because the kernel enumerates /dev/media* in non-deterministic order on each
# boot, we scan all media devices to find the one hosting the ov7251 sensor,
# then enable the media-controller link and set the bus format.
#
# The discovered capture device node is written to /run/surface_ir_bridge_dev
# (root-owned, mode 0644) so ir_bridge can find it at runtime without
# relying on world-writable /tmp.

set -euo pipefail

RUNTIME_FILE="/run/surface_ir_bridge_dev"

for dev in /dev/media*; do
    if media-ctl -d "$dev" -p 2>/dev/null | grep -q "ov7251"; then
        # Enable the link: ov7251 → ipu3-csi2
        media-ctl -d "$dev" -l '"ov7251 3-0060":0->"ipu3-csi2 2":0[1]'

        # Set the bus format on the CSI-2 sink pad
        media-ctl -d "$dev" -V '"ipu3-csi2 2":0 [fmt:Y10_1X10/640x480]'

        # Discover the actual /dev/videoN for the CIO2 capture node
        VIDEO_NODE=$(media-ctl -d "$dev" -e "ipu3-cio2 2")

        # Write to a root-owned runtime file (not /tmp!)
        echo "$VIDEO_NODE" > "$RUNTIME_FILE"
        chmod 0644 "$RUNTIME_FILE"

        # Increase sensor exposure and gain for much brighter images
        # The OV7251 is extremely dark by default
        OV_SUBDEV=""
        for sub in /sys/class/video4linux/v4l-subdev*; do
            if grep -q "ov7251" "$sub/name" 2>/dev/null; then
                OV_SUBDEV="/dev/$(basename "$sub")"
                break
            fi
        done
        if [ -n "$OV_SUBDEV" ]; then
            v4l2-ctl -d "$OV_SUBDEV" --set-ctrl exposure=1000 2>/dev/null || true
            v4l2-ctl -d "$OV_SUBDEV" --set-ctrl analogue_gain=32 2>/dev/null || true
            echo "setup_ipu3: boosted exposure on $OV_SUBDEV"
        fi

        echo "setup_ipu3: sensor on $dev, capture node $VIDEO_NODE"
        exit 0
    fi
done

echo "setup_ipu3: ERROR: ov7251 sensor not found on any /dev/media* device" >&2
exit 1
