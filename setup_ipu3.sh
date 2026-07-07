#!/bin/bash
for dev in /dev/media*; do
    if media-ctl -d $dev -p 2>/dev/null | grep -q "ov7251"; then
        media-ctl -d $dev -l '"ov7251 3-0060":0->"ipu3-csi2 2":0[1]'
        media-ctl -d $dev -V '"ipu3-csi2 2":0 [fmt:Y10_1X10/640x480]'
        exit 0
    fi
done
exit 1
