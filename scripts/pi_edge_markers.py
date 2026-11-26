#!/usr/bin/env python3
"""Reference edges for measure_offset.py: toggles a Raspberry Pi GPIO at random 150-350 ms intervals and pushes an
LSL marker (stream "EdgeMarkers") stamped at each edge. Wire the GPIO through a divider to the audio input.

  python3 pi_edge_markers.py [GPIO=16] [SECONDS=300]

Needs gpiozero (preinstalled on Raspberry Pi OS) and pylsl (pip install pylsl; on a Pi it may also need liblsl
installed from https://github.com/sccn/liblsl/releases).
"""
import random
import sys
import time

from gpiozero import DigitalOutputDevice
from pylsl import StreamInfo, StreamOutlet, local_clock

pin = int(sys.argv[1]) if len(sys.argv) > 1 else 16
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 300
out = StreamOutlet(StreamInfo("EdgeMarkers", "Markers", 1, 0, "int32", f"pi-gpio{pin}"))
gpio = DigitalOutputDevice(pin)
print(f"Toggling GPIO{pin} for {secs:.0f} s; start recording now.")
end, v = time.monotonic() + secs, 0
while time.monotonic() < end:
    time.sleep(random.uniform(0.15, 0.35))  # random spacing: an edge cannot be matched to the wrong one
    v ^= 1
    t0 = local_clock()
    gpio.value = v
    t1 = local_clock()
    out.push_sample([v], (t0 + t1) / 2)  # bracket the write; its duration (tens of us in Python) bounds the error
gpio.off()
