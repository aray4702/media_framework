#!/usr/bin/env python3
"""Compares web exports with the macOS reference exports of the same scenes: frame counts,
video PSNR (ffmpeg), and the audio's difference after aligning the two AAC encoders' delays.
Usage: compare_exports.py dir scene... (dir holds <scene>.mp4 from run_spike.mjs with EXPORT=dir,
and <scene>-mac.mp4 from export_reference)."""

import json
import re
import subprocess
import sys

import numpy as np


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type,nb_frames,duration", "-of", "json", path],
                         capture_output=True, text=True, check=True).stdout
    return {s["codec_type"]: s for s in json.loads(out)["streams"]}


def psnr(a, b):
    err = subprocess.run(["ffmpeg", "-v", "info", "-i", a, "-i", b, "-lavfi", "[0:v][1:v]psnr", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    m = re.search(r"average:([0-9.inf]+)", err)
    return m.group(1) if m else "?"


def pcm(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", "48000", "-f", "s16le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.int16).astype(np.float64)


def audio(a, b):
    """The lag (samples) that best aligns b to a, and the signal-to-difference ratio there (dB)."""
    x, y = pcm(a), pcm(b)
    if not len(x) or not len(y):
        return None, None
    n = min(len(x), len(y), 48000 * 4)
    best, best_lag = None, 0
    for lag in range(-3000, 3001, 1):
        xs, ys = (x[lag:n], y[: n - lag]) if lag >= 0 else (x[: n + lag], y[-lag:n])
        d = np.sum((xs - ys) ** 2)
        if best is None or d < best:
            best, best_lag = d, lag
    lag = best_lag
    xs, ys = (x[lag:n], y[: n - lag]) if lag >= 0 else (x[: n + lag], y[-lag:n])
    signal = np.sum(xs ** 2)
    return lag, 10 * np.log10(signal / max(np.sum((xs - ys) ** 2), 1e-9))


def main():
    folder, scenes = sys.argv[1], sys.argv[2:]
    print(f"{'scene':10} {'web frames':>10} {'mac frames':>10} {'PSNR dB':>8} {'audio lag':>9} {'audio SDR dB':>12}")
    for s in scenes:
        web, mac = f"{folder}/{s}.mp4", f"{folder}/{s}-mac.mp4"
        pw, pm = probe(web), probe(mac)
        lag, sdr = audio(web, mac) if "audio" in pw and "audio" in pm else (None, None)
        print(f"{s:10} {pw['video']['nb_frames']:>10} {pm['video']['nb_frames']:>10} {psnr(web, mac):>8} "
              f"{lag if lag is not None else '-':>9} {(f'{sdr:.1f}' if sdr is not None else '-'):>12}")


if __name__ == "__main__":
    main()
