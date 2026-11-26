#!/usr/bin/env python3
# /// script
# dependencies = ["numpy", "pyxdf"]
# ///
"""Measure AudioCapture's timestamp offset (--ts-offset-us / timestamp_offset_us) from an XDF recording.

Record, with LabRecorder, the AudioCapture stream (offset 0) and a reference that sees the same electrical edges:
  - an analog stream on the same computer (e.g. MCCOutletCLI --raw) with the signal on one channel, or
  - a marker stream with one sample per edge, e.g. pi_edge_markers.py on a Raspberry Pi (LSL clock sync maps it).
Prints the median of (audio edge time - reference edge time) in microseconds: the value to supply.

  uv run measure_offset.py rec.xdf AUDIO_STREAM REF_STREAM [--audio-ch 0] [--ref-ch 0] [--ref-offset-us 0]
  uv run measure_offset.py --selftest
"""
import argparse
import sys

import numpy as np


def edges(t, x, gap=0.05):
    """Edge times in x (sampled at t): the half-way crossing between the level before an edge and its peak.
    Works for either polarity, AC-coupled (drooping) inputs and clipped signals. Edges must be > gap s apart."""
    x = np.asarray(x, float)
    sr = (len(t) - 1) / (t[-1] - t[0])
    k, w, pk = max(1, int(sr * 1e-3)), int(sr * 8e-3), int(sr * 5e-3)  # 1 ms step, 8 ms baseline, 5 ms peak window
    step = np.abs(x[k:] - x[:-k])
    hits = np.flatnonzero(step > 0.3 * np.percentile(step, 99.9)) + k
    if not len(hits):
        return np.array([])
    starts = hits[np.r_[True, np.diff(hits) > gap * sr]]
    out = []
    for j in starts[(starts > w + k) & (starts < len(x) - pk)]:
        b = np.median(x[j - k - w:j - k])
        seg = x[j - k:j + pk]
        p = seg[np.argmax(np.abs(seg - b))]
        above = np.flatnonzero((seg - (b + p) / 2) * np.sign(p - b) > 0)
        if not len(above) or above[0] == 0:
            continue
        i = j - k + above[0]
        lvl = (b + p) / 2
        out.append(t[i - 1] + (t[i] - t[i - 1]) * (lvl - x[i - 1]) / (x[i] - x[i - 1]))
    return np.array(out)


def match(audio, ref, max_lag=0.1):
    """audio edge - latest reference edge before it (allowing 5 ms early), for lags within (-5 ms, max_lag)."""
    i = np.searchsorted(ref, audio + 0.005) - 1
    ok = i >= 0
    lag = audio[ok] - ref[i[ok]]
    return lag[(lag > -0.005) & (lag < max_lag)]


def report(lag, n_audio, n_ref):
    us = lag * 1e6
    p1, med, p99 = np.percentile(us, [1, 50, 99])
    print(f"{len(lag)} of {n_audio} audio edges matched ({n_ref} reference edges)")
    print(f"audio - reference: median {med:.0f} us, mean {us.mean():.0f}, SD {us.std():.0f}, p1 {p1:.0f}, p99 {p99:.0f}")
    print(f"=> --ts-offset-us {med:.0f}   (or timestamp_offset_us={med:.0f} in the GUI config)")
    return med


def selftest():
    rng = np.random.default_rng(1)
    te = np.cumsum(rng.uniform(0.15, 0.35, 100)) + 1.0  # true edge times
    lvl = lambda t: (np.searchsorted(te, t) % 2).astype(float)  # square wave
    tr = np.arange(0, te[-1] + 1, 1 / 50000)  # DC-coupled reference at 50 kHz, stamped 30 us late
    ref = edges(tr + 30e-6, lvl(tr) + rng.normal(0, 0.01, len(tr)))
    ta = np.arange(0, te[-1] + 1, 1 / 48000)  # audio stamped 520 us late, then AC-coupled (0.2 s), inverted, clipped
    x, y, c = lvl(ta - 520e-6), np.zeros(len(ta)), np.exp(-1 / (48000 * 0.2))
    for i in range(1, len(x)):
        y[i] = c * (y[i - 1] + x[i] - x[i - 1])
    lag = match(edges(ta, -np.clip(y, -0.6, 0.6) + rng.normal(0, 0.01, len(y))), ref)
    med = np.median(lag) * 1e6
    assert len(lag) >= 98 and abs(med - 490) < 15, (len(lag), med)  # 520 - 30 us, within a third of a sample
    print(f"selftest ok: {len(lag)} edges, median {med:.0f} us (expected 490)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xdf")
    ap.add_argument("audio")
    ap.add_argument("ref")
    ap.add_argument("--audio-ch", type=int, default=0)
    ap.add_argument("--ref-ch", type=int, default=0)
    ap.add_argument("--ref-offset-us", type=float, default=0, help="known latency of the reference not already "
                    "removed from its timestamps (e.g. an MCC stream recorded with its own offset at 0)")
    if "--selftest" in sys.argv:
        return selftest()
    args = ap.parse_args()
    import pyxdf

    # raw timestamps (no dejitter); clock offsets applied, so a reference from another host is mapped onto this one
    streams, _ = pyxdf.load_xdf(args.xdf, dejitter_timestamps=False)
    by_name = {s["info"]["name"][0]: s for s in streams}
    for n in (args.audio, args.ref):
        if n not in by_name:
            sys.exit(f"stream '{n}' not in {args.xdf}; streams: {', '.join(by_name)}")
    a, r = by_name[args.audio], by_name[args.ref]
    audio = edges(a["time_stamps"], np.asarray(a["time_series"])[:, args.audio_ch])
    if float(r["info"]["nominal_srate"][0]) == 0:  # marker stream: each sample is an edge
        ref = np.asarray(r["time_stamps"], float)
    else:
        ref = edges(r["time_stamps"], np.asarray(r["time_series"], float)[:, args.ref_ch])
    ref = ref - args.ref_offset_us * 1e-6
    lag = match(audio, ref)
    if len(lag) < 20:
        sys.exit(f"only {len(lag)} matched edges ({len(audio)} audio, {len(ref)} reference): check channels and wiring")
    report(lag, len(audio), len(ref))


if __name__ == "__main__":
    main()
