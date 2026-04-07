#!/usr/bin/env python3
import argparse
import csv
import os
import statistics
import subprocess
import time
import shutil
from typing import Optional, Tuple, Callable, List
from dataclasses import dataclass
from pathlib import Path

try:
    import matplotlib.pyplot as plt
except ImportError:
    print("matplotlib est requis. Installez-le avec: pip install matplotlib")
    raise


@dataclass
class TestCase:
    name: str
    args: list[str]
    timeout_s: int = 20


# Liste des tests benchmark.
ALL_TESTS: list[TestCase] = [
    TestCase("01-main", []),
    TestCase("02-switch", []),
    TestCase("03-equity", []),
    TestCase("11-join", []),
    TestCase("12-join-main", []),
    TestCase("13-join-switch", []),
    TestCase("21-create-many", ["200"]),
    TestCase("22-create-many-recursive", ["200"]),
    TestCase("23-create-many-once", ["200"]),
    TestCase("31-switch-many", ["8", "2000"]),
    TestCase("32-switch-many-join", ["80", "2000"]),
    TestCase("33-switch-many-cascade", ["40", "30"]),
    TestCase("51-fibonacci", ["18"]),
    TestCase("61-mutex", ["40"]),
    TestCase("62-mutex", ["10"]),
    TestCase("63-mutex-equity", []),
    TestCase("64-mutex-join", []),
    #TestCase("71-preemption", ["5"]),
    #TestCase("81-deadlock", []),
    TestCase("sum", []),
    TestCase("sort", []),
    TestCase("reduction", []),
    TestCase("matrix_mul", []),
]


def run_cmd(cmd: list[str], cwd: Path) -> None:
    result = subprocess.run(cmd, cwd=str(cwd), check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Commande echouee ({result.returncode}): {' '.join(cmd)}")


def timed_run(cmd: list[str], cwd: Path, env: dict[str, str], timeout_s: int) -> tuple[bool, float, str]:
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_s,
            check=False,
        )
        dt = time.perf_counter() - t0
        ok = proc.returncode == 0
        if ok:
            return True, dt, ""
        detail = (
            f"code={proc.returncode}; "
            f"stdout={proc.stdout[-200:].strip()}; "
            f"stderr={proc.stderr[-200:].strip()}"
        )
        return False, dt, detail
    except subprocess.TimeoutExpired:
        dt = time.perf_counter() - t0
        return False, dt, f"timeout>{timeout_s}s"


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["test", "impl", "run_id", "ok", "time_s", "args", "detail"],
        )
        writer.writeheader()
        writer.writerows(rows)


def median_or_nan(values: list[float]) -> float:
    return statistics.median(values) if values else float("nan")


def default_core_counts() -> list[int]:
    cpu_count = os.cpu_count() or 1
    core_counts = []
    value = 1
    while value < cpu_count:
        core_counts.append(value)
        value *= 2
    if cpu_count not in core_counts:
        core_counts.append(cpu_count)
    return core_counts


def parse_core_counts(value: Optional[str]) -> List[int]:
    if not value:
        return default_core_counts()
    cpu_count = os.cpu_count() or 1
    core_counts = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        core_count = int(item)
        if core_count < 1:
            raise ValueError("Chaque valeur de --cores doit etre >= 1")
        if core_count > cpu_count:
            raise ValueError(f"Chaque valeur de --cores doit etre <= {cpu_count}")
        core_counts.append(core_count)
    if not core_counts:
        raise ValueError("--cores ne contient aucune valeur valide")
    return sorted(set(core_counts))


def get_affinity_prefix(core_count: int) -> Tuple[List[str], Optional[Callable[[], None]]]:
    if hasattr(os, "sched_setaffinity"):
        core_set = set(range(core_count))

        def set_affinity() -> None:
            os.sched_setaffinity(0, core_set)

        return [], set_affinity

    if shutil.which("taskset"):
        return ["taskset", "-c", f"0-{core_count - 1}"], None

    return [], None


def plot_scaling(
    path: Path,
    selected_tests: list[TestCase],
    core_counts: list[int],
    custom_times: dict[str, dict[int, list[float]]],
    pthread_times: dict[str, dict[int, list[float]]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    x = list(range(len(tests)))
    w = 0.42

    if len(selected_tests) == 1:
        fig, axes = plt.subplots(1, 1, figsize=(8, 5), sharex=True)
        axes = [axes]
    else:
        fig, axes = plt.subplots(2, 2, figsize=(14, 9), sharex=True)
        axes = [axis for row in axes for axis in row]

    for axis in axes[len(selected_tests):]:
        axis.axis("off")

    for axis, test in zip(axes, selected_tests):
        y_custom = [median_or_nan(custom_times[test.name][core]) for core in core_counts]
        y_pthread = [median_or_nan(pthread_times[test.name][core]) for core in core_counts]

        axis.plot(core_counts, y_custom, marker="o", linewidth=2, label="thread.c")
        axis.plot(core_counts, y_pthread, marker="o", linewidth=2, label="pthread")
        axis.set_title(test.name)
        axis.set_xlabel("Nombre de coeurs utilises")
        axis.set_ylabel("Temps median (s)")
        axis.grid(True, axis="y", alpha=0.3)
        axis.set_xticks(core_counts)
        axis.legend()

    fig.suptitle("Temps d'execution en fonction du nombre de coeurs")
    fig.tight_layout()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def timed_run_limited(
    cmd: list[str],
    cwd: Path,
    env: dict[str, str],
    timeout_s: int,
    core_count: Optional[int] = None
) -> tuple[bool, float, str]:
    if core_count is None:
        return timed_run(cmd, cwd, env, timeout_s)

    prefix, preexec = get_affinity_prefix(core_count)
    run_cmd = prefix + cmd
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            run_cmd,
            cwd=str(cwd),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_s,
            check=False,
            preexec_fn=preexec,
        )
        dt = time.perf_counter() - t0
        ok = proc.returncode == 0
        if ok:
            return True, dt, ""
        detail = (
            f"code={proc.returncode}; "
            f"stdout={proc.stdout[-200:].strip()}; "
            f"stderr={proc.stderr[-200:].strip()}"
        )
        return False, dt, detail
    except subprocess.TimeoutExpired:
        dt = time.perf_counter() - t0
        return False, dt, f"timeout>{timeout_s}s"


def main() -> int:

    parser = argparse.ArgumentParser(description="Benchmark simple maison vs pthread")
    parser.add_argument("--repo", default=".")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=20, help="Timeout par test en secondes")
    parser.add_argument("--csv", default="bench/results.csv")
    parser.add_argument("--png", default="bench/results.png")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--custom-tests", action="store_true", help="N'exécute que sum, sort, reduction, matrix_mul")
    args = parser.parse_args()

    if args.runs < 1:
        print("--runs doit etre >= 1")
        return 2
    if args.timeout < 1:
        print("--timeout doit etre >= 1")
        return 2

    repo = Path(args.repo).resolve()
    bin_dir = repo / "bin"

    if not args.skip_build:
        print("[build] make all pthreads", flush=True)
        run_cmd(["make", "--no-print-directory", "all", "pthreads"], cwd=repo)

    env = os.environ.copy()
    ld_path = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = str(repo) + ((":" + ld_path) if ld_path else "")

    # Sélection des tests à exécuter
    if args.custom_tests:
        selected_tests = [
            TestCase("sum", []),
            TestCase("sort", []),
            TestCase("reduction", []),
            TestCase("matrix_mul", []),
        ]
    else:
        selected_tests = ALL_TESTS

    rows: list[dict[str, str]] = []
    custom_times: dict[str, list[float]] = {t.name: [] for t in selected_tests}
    pthread_times: dict[str, list[float]] = {t.name: [] for t in selected_tests}

    total = len(selected_tests) * args.runs * 2
    done = 0

    for test in selected_tests:
        for run_id in range(1, args.runs + 1):
            for impl, exe in [
                ("custom", bin_dir / test.name),
                ("pthread", bin_dir / f"{test.name}-pthread"),
            ]:
                done += 1
                cmd = [str(exe)] + test.args
                print(f"[{done}/{total}] {test.name} {impl}", flush=True)
                ok, dt, detail = timed_run(cmd, cwd=repo, env=env, timeout_s=args.timeout)
                rows.append(
                    {
                        "test": test.name,
                        "impl": impl,
                        "run_id": str(run_id),
                        "ok": "1" if ok else "0",
                        "time_s": f"{dt:.6f}",
                        "args": " ".join(test.args),
                        "detail": detail,
                    }
                )
                if ok:
                    if impl == "custom":
                        custom_times[test.name].append(dt)
                    else:
                        pthread_times[test.name].append(dt)
                else:
                    print(f"  -> echec ({impl} {test.name}): {detail}", flush=True)

    csv_path = (repo / args.csv).resolve()
    png_path = (repo / args.png).resolve()
    write_csv(csv_path, rows)

    test_names = [t.name for t in selected_tests]
    y_custom = [median_or_nan(custom_times[n]) for n in test_names]
    y_pthread = [median_or_nan(pthread_times[n]) for n in test_names]
    plot(png_path, test_names, y_custom, y_pthread)

    ok_count = sum(1 for r in rows if r["ok"] == "1")
    print(f"CSV : {csv_path}")
    print(f"Graphe : {png_path}")
    print(f"Executions reussies: {ok_count}/{total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
