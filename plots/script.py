#!/usr/bin/env python3
import argparse
import csv
import os
import shutil
import statistics
import subprocess
import time
from pathlib import Path

import matplotlib.pyplot as plt


TESTS = [
    "21-create-many",
    "22-create-many-recursive",
    "23-create-many-once",
    "61-mutex",
]

THREADS = [100, 500, 1000, 2000]


def parse_graph_seconds(output: str) -> float | None:
    for line in output.splitlines():
        if line.startswith("GRAPH;"):
            parts = line.strip().split(";")
            try:
                return int(parts[-1]) / 1_000_000
            except (ValueError, IndexError):
                return None
    return None


def run_once(binary: Path, n: int, pin_core: bool) -> float | None:
    cmd = [str(binary), str(n)]
    if pin_core and shutil.which("taskset"):
        cmd = ["taskset", "-c", "0"] + cmd

    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    except Exception as exc:
        print(f"[ERROR] echec lancement {binary.name}({n}) : {exc}")
        return None

    if proc.returncode != 0:
        print(f"[ERROR] {binary.name}({n}) code={proc.returncode}")
        return None

    output = proc.stdout + proc.stderr
    graph_s = parse_graph_seconds(output)
    if graph_s is not None:
        return graph_s

    return time.perf_counter() - t0


def average_time(binary: Path, n: int, runs: int, pin_core: bool) -> float | None:
    values: list[float] = []
    for _ in range(runs):
        value = run_once(binary, n, pin_core)
        if value is not None:
            values.append(value)
    if not values:
        return None
    return statistics.mean(values)


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark custom vs pthread")
    parser.add_argument("--repo", default=".")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--no-pin", action="store_true", help="Désactive taskset -c 0")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    bin_dir = repo / "bin"
    results_dir = repo / "results"
    results_dir.mkdir(exist_ok=True)

    if not args.skip_build:
        print("[BUILD] make all pthreads")
        build = subprocess.run(["make", "all", "pthreads"], cwd=repo, check=False)
        if build.returncode != 0:
            print("[ERROR] build échoué")
            return 1

    pin_core = not args.no_pin
    rows: list[list[str]] = [["test", "n", "custom_s", "pthread_s"]]

    for test in TESTS:
        print(f"\n=== Test: {test} ===")
        custom_bin = bin_dir / test
        pthread_bin = bin_dir / f"{test}-pthread"

        if not custom_bin.exists() or not pthread_bin.exists():
            print(f"[SKIP] binaire manquant pour {test}")
            continue

        custom_times: list[float] = []
        pthread_times: list[float] = []
        x_values: list[int] = []

        for n in THREADS:
            t_custom = average_time(custom_bin, n, args.runs, pin_core)
            t_pthread = average_time(pthread_bin, n, args.runs, pin_core)
            print(f"n={n:4d} | custom={t_custom} s | pthread={t_pthread} s")

            if t_custom is None or t_pthread is None:
                continue

            x_values.append(n)
            custom_times.append(t_custom)
            pthread_times.append(t_pthread)
            rows.append([test, str(n), f"{t_custom:.6f}", f"{t_pthread:.6f}"])

        if not x_values:
            print(f"[SKIP] aucune mesure valide pour {test}")
            continue

        plt.figure()
        plt.plot(x_values, custom_times, marker="o", label="custom")
        plt.plot(x_values, pthread_times, marker="o", label="pthread")
        plt.xlabel("Nombre de threads")
        plt.ylabel("Temps (s)")
        plt.title(f"Performance - {test}")
        plt.grid(True)
        plt.legend()
        plt.tight_layout()
        plt.savefig(results_dir / f"{test}.png", dpi=160)
        plt.close()

    with (results_dir / "results.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerows(rows)

    print(f"\nRésultats écrits dans: {results_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())