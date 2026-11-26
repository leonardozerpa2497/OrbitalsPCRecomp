"""Frame statistics of bench runs over a time window (A/B comparisons on the same scripted co-op scene).

  python bench_stats.py [--from S] [--to S] [--late 17.7] <run dir>[:label] ...

A run dir holds the session's frames.csv (one line per present, ms since start) and Orbitals.log. Window times are seconds
on the frames.csv clock. Per run: FPS, frame interval p50 / p95 / p99, the share of intervals over --late ms (17.7 = a
missed 60 Hz frame with 6 % margin) and over 30 ms (spikes), the worst interval, the NVN translation thread's CPU ms a frame
('[nvn] translation thread up to frame' lines) and the GPU ms a frame ('GPU time up to frame' lines) logged inside the window.
"""
import csv, os, re, sys, statistics

TR = re.compile(r"^\[\s*([0-9.]+)\].*translation thread up to frame \d+: ([0-9.]+) ms a frame translating \(CPU ([0-9.]+) ms\)")
GPU = re.compile(r"^\[\s*([0-9.]+)\].*GPU time up to frame \d+: ([0-9.]+) ms per frame")


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * len(v)))] if v else float("nan")


def run_stats(run, t0, t1, late):
    ts = []
    with open(os.path.join(run, "frames.csv")) as f:
        for row in csv.DictReader(f):
            t = float(row["ms"]) / 1000
            if t0 <= t <= t1:
                ts.append(t)
    iv = [(b - a) * 1000 for a, b in zip(ts, ts[1:])]
    fps = (len(ts) - 1) / (ts[-1] - ts[0]) if len(ts) > 1 else float("nan")
    tr, gpu = [], []
    log = os.path.join(run, "Orbitals.log")
    if os.path.isfile(log):
        for line in open(log, errors="replace"):
            m = TR.match(line)
            if m and t0 <= float(m.group(1)) <= t1:
                tr.append(float(m.group(3)))
            m = GPU.match(line)
            if m and t0 <= float(m.group(1)) <= t1:
                gpu.append(float(m.group(2)))
    n = len(iv) or 1
    return dict(fps=fps, p50=pct(iv, 50), p95=pct(iv, 95), p99=pct(iv, 99),
                late=100.0 * sum(x > late for x in iv) / n, spikes=100.0 * sum(x > 30 for x in iv) / n,
                worst=max(iv) if iv else float("nan"), tr=statistics.mean(tr) if tr else float("nan"),
                gpu=statistics.mean(gpu) if gpu else float("nan"), frames=len(ts))


def main():
    a = sys.argv[1:]
    t0, t1, late = 0.0, 1e9, 17.7
    for flag in ("--from", "--to", "--late"):
        if flag in a:
            i = a.index(flag); v = float(a[i + 1]); del a[i:i + 2]
            t0, t1, late = (v, t1, late) if flag == "--from" else (t0, v, late) if flag == "--to" else (t0, t1, v)
    print(f"window {t0:.0f}-{t1:.0f} s, late = interval > {late} ms")
    print(f"{'run':34} {'FPS':>6} {'p50':>6} {'p95':>6} {'p99':>6} {'late%':>6} {'>30%':>6} {'worst':>7} {'transl':>6} {'GPU':>6} {'frames':>6}")
    for arg in a:
        run, _, label = arg.partition(":") if not os.path.isdir(arg) else (arg, "", "")
        s = run_stats(run, t0, t1, late)
        name = label or os.path.basename(run.rstrip("/\\"))
        print(f"{name:34} {s['fps']:6.2f} {s['p50']:6.2f} {s['p95']:6.2f} {s['p99']:6.2f} {s['late']:6.2f} {s['spikes']:6.2f} "
              f"{s['worst']:7.1f} {s['tr']:6.2f} {s['gpu']:6.2f} {s['frames']:6d}")


if __name__ == "__main__":
    main()
