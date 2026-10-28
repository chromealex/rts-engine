#!/usr/bin/env python3
"""Headless sim time of two rts_headless builds (min of N alternating runs per game and seed).
Usage: tests/checksums/perf.py BASE_BIN NEW_BIN [--runs 3] [--seeds 1,2,3] [--ir SECONDS] game_dir...
The Lua port uses it to compare a module before/after (the machine is shared: take minimums).
--ir N: instead of wall time, count executed instructions (valgrind callgrind, deterministic) of the
first N game seconds: exact numbers on a busy machine (whole process, setup included)."""
import re, subprocess, sys

def sim_ms(binary, game, seed):
    out = subprocess.run([binary, "--project", game, "--seed", str(seed), "--quiet"], capture_output=True, text=True).stdout
    m = re.search(r"sim=(\d+) ms", out)
    return int(m.group(1)) if m else -1

def instructions(binary, game, seed, seconds):
    r = subprocess.run(["valgrind", "--tool=callgrind", "--callgrind-out-file=/dev/null", binary, "--project", game, "--seed", str(seed),
                        "--quiet", "--max-seconds", str(seconds)], capture_output=True, text=True)
    m = re.search(r"Collected : (\d+)", r.stderr)
    return int(m.group(1)) // 1000000 if m else -1   # millions

def main():
    args = sys.argv[1:]
    runs, seeds = 3, [1, 2, 3]
    if "--runs" in args: i = args.index("--runs"); runs = int(args[i + 1]); del args[i:i + 2]
    if "--seeds" in args: i = args.index("--seeds"); seeds = [int(s) for s in args[i + 1].split(",")]; del args[i:i + 2]
    ir = 0
    if "--ir" in args: i = args.index("--ir"); ir = int(args[i + 1]); del args[i:i + 2]
    base, new, games = args[0], args[1], args[2:]
    tb = tn = 0
    for g in games:
        b = n = 0
        for s in seeds:
            bs, ns = [], []
            if ir:
                bs.append(instructions(base, g, s, ir)); ns.append(instructions(new, g, s, ir))
            else:
                for _ in range(runs):
                    bs.append(sim_ms(base, g, s)); ns.append(sim_ms(new, g, s))
            b += min(bs); n += min(ns)
        tb += b; tn += n
        unit = "M instr" if ir else "ms"
        print(f"{g:40s} {b:9d} -> {n:9d} {unit}  {100.0 * (n - b) / max(b, 1):+6.1f}%", flush=True)
    print(f"{'TOTAL':40s} {tb:9d} -> {tn:9d}  {100.0 * (tn - tb) / max(tb, 1):+6.1f}%")

main()
