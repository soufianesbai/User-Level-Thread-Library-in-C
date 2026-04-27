#!/usr/bin/env python3
import argparse
import csv
import os
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    import matplotlib.pyplot as plt
except ImportError:
    print("matplotlib est requis. Installez-le avec: pip install matplotlib", file=sys.stderr)
    raise


@dataclass(frozen=True)
class BenchCase:
    name: str
    args: list[str]
    timeout_s: int = 90


BENCH_CASES: list[BenchCase] = [
    BenchCase("31-switch-many", ["200", "200"]),
    BenchCase("32-switch-many-join", ["200", "200"]),
    BenchCase("33-switch-many-cascade", ["120", "150"]),
    BenchCase("sum", []),
]


def run_cmd(cmd: list[str], cwd: Path) -> None:
    proc = subprocess.run(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "Commande échouée:\n"
            f"  cmd: {' '.join(cmd)}\n"
            f"  cwd: {cwd}\n"
            f"  code: {proc.returncode}\n"
            f"  stdout: {proc.stdout[-400:]}\n"
            f"  stderr: {proc.stderr[-400:]}"
        )


def timed_run(cmd: list[str], cwd: Path, timeout_s: int) -> float:
    start = time.perf_counter()
    proc = subprocess.run(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout_s)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(
            "Exécution échouée:\n"
            f"  cmd: {' '.join(cmd)}\n"
            f"  cwd: {cwd}\n"
            f"  code: {proc.returncode}\n"
            f"  stdout: {proc.stdout[-400:]}\n"
            f"  stderr: {proc.stderr[-400:]}"
        )
    return elapsed


def short_revision(repo: Path, rev: str) -> str:
    return (
        subprocess.check_output(["git", "rev-parse", "--short", rev], cwd=str(repo), text=True)
        .strip()
    )


def resolve_rev(repo: Path, rev: str) -> str:
    return (
        subprocess.check_output(["git", "rev-parse", rev], cwd=str(repo), text=True)
        .strip()
    )


def add_worktree(repo: Path, path: Path, rev: str) -> None:
    if path.exists():
        shutil.rmtree(path)
    run_cmd(["git", "worktree", "add", "--detach", str(path), rev], cwd=repo)


def remove_worktree(repo: Path, path: Path) -> None:
    if path.exists():
        run_cmd(["git", "worktree", "remove", "--force", str(path)], cwd=repo)


def build_tree(tree: Path) -> None:
    run_cmd(["make", "clean"], cwd=tree)
    run_cmd(["make", "all", "pthreads"], cwd=tree)


def benchmark_tree(tree: Path, cases: Iterable[BenchCase], runs: int) -> dict[str, list[float]]:
    results: dict[str, list[float]] = {case.name: [] for case in cases}
    for case in cases:
        exe = tree / "bin" / case.name
        cmd = [str(exe)] + case.args
        for run_idx in range(runs):
            dt = timed_run(cmd, cwd=tree, timeout_s=case.timeout_s)
            results[case.name].append(dt)
            print(f"[{tree.name}] {case.name} run {run_idx + 1}/{runs}: {dt:.6f}s", flush=True)
    return results


def median(values: list[float]) -> float:
    return statistics.median(values)


def write_csv(
    csv_path: Path,
    cases: list[BenchCase],
    before_results: dict[str, list[float]],
    after_results: dict[str, list[float]],
) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["test", "args", "before_median_s", "after_median_s", "speedup_x"])
        for case in cases:
            before_m = median(before_results[case.name])
            after_m = median(after_results[case.name])
            speedup = before_m / after_m if after_m > 0 else float("inf")
            writer.writerow([case.name, " ".join(case.args), f"{before_m:.6f}", f"{after_m:.6f}", f"{speedup:.3f}"])


def plot_runtime(
    png_path: Path,
    cases: list[BenchCase],
    before_label: str,
    after_label: str,
    before_results: dict[str, list[float]],
    after_results: dict[str, list[float]],
) -> None:
    png_path.parent.mkdir(parents=True, exist_ok=True)
    labels = [case.name for case in cases]
    before_medians = [median(before_results[case.name]) for case in cases]
    after_medians = [median(after_results[case.name]) for case in cases]

    x = list(range(len(cases)))
    width = 0.36

    fig, ax = plt.subplots(figsize=(11, 5))
    ax.bar([i - width / 2 for i in x], before_medians, width=width, label=before_label)
    ax.bar([i + width / 2 for i in x], after_medians, width=width, label=after_label)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right")
    ax.set_ylabel("Temps médian (s)")
    ax.set_title("Comparaison avant/après: coût des opérations orientées switch de contexte")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(png_path, dpi=180)
    plt.close(fig)


def plot_speedup(
    png_path: Path,
    cases: list[BenchCase],
    before_results: dict[str, list[float]],
    after_results: dict[str, list[float]],
) -> None:
    png_path.parent.mkdir(parents=True, exist_ok=True)
    labels = [case.name for case in cases]
    speedups = []
    for case in cases:
        before_m = median(before_results[case.name])
        after_m = median(after_results[case.name])
        speedups.append(before_m / after_m if after_m > 0 else float("inf"))

    fig, ax = plt.subplots(figsize=(10, 4.5))
    bars = ax.bar(labels, speedups)
    ax.axhline(1.0, linestyle="--", linewidth=1)
    ax.set_ylabel("Accélération (x)")
    ax.set_title("Accélération (baseline swapcontext / implémentation ASM)")
    ax.grid(True, axis="y", alpha=0.3)
    for bar, value in zip(bars, speedups):
        ax.text(bar.get_x() + bar.get_width() / 2, value, f"{value:.2f}x", ha="center", va="bottom", fontsize=9)
    fig.tight_layout()
    fig.savefig(png_path, dpi=180)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare la baseline swapcontext avec la révision implémentation ASM du switch de contexte."
    )
    parser.add_argument("--repo", default=".", help="Chemin du dépôt")
    parser.add_argument("--target", default="003e3c1", help="Révision Git de l'implémentation ASM")
    parser.add_argument("--runs", type=int, default=4, help="Nombre de runs par test")
    parser.add_argument("--output-dir", default="analysis/builds/06_context_switch_asm", help="Dossier de sortie")
    args = parser.parse_args()

    if args.runs < 1:
        print("--runs doit être >= 1", file=sys.stderr)
        return 2

    repo = Path(args.repo).resolve()
    output_dir = (repo / args.output_dir).resolve()
    tmp_root = output_dir / "_worktrees"

    target_rev = resolve_rev(repo, args.target)
    parent_rev = resolve_rev(repo, f"{args.target}^")
    target_short = short_revision(repo, target_rev)
    parent_short = short_revision(repo, parent_rev)

    before_tree = tmp_root / f"before_{parent_short}"
    after_tree = tmp_root / f"after_{target_short}"

    output_dir.mkdir(parents=True, exist_ok=True)
    tmp_root.mkdir(parents=True, exist_ok=True)

    try:
        print(f"[git] parent={parent_short} target={target_short}", flush=True)
        add_worktree(repo, before_tree, parent_rev)
        add_worktree(repo, after_tree, target_rev)

        print("[build] before", flush=True)
        build_tree(before_tree)
        print("[build] after", flush=True)
        build_tree(after_tree)

        print("[bench] before", flush=True)
        before_results = benchmark_tree(before_tree, BENCH_CASES, args.runs)
        print("[bench] after", flush=True)
        after_results = benchmark_tree(after_tree, BENCH_CASES, args.runs)

        csv_path = output_dir / "results.csv"
        runtime_png = output_dir / "runtime_comparison.png"
        speedup_png = output_dir / "speedup.png"

        write_csv(csv_path, BENCH_CASES, before_results, after_results)
        plot_runtime(
            runtime_png,
            BENCH_CASES,
            f"Baseline swapcontext ({parent_short})",
            f"Implémentation ASM ({target_short})",
            before_results,
            after_results,
        )
        plot_speedup(speedup_png, BENCH_CASES, before_results, after_results)

        print(f"CSV: {csv_path}")
        print(f"PNG: {runtime_png}")
        print(f"PNG: {speedup_png}")
        return 0
    finally:
        try:
            remove_worktree(repo, before_tree)
        except Exception as exc:
            print(f"[warn] remove worktree before failed: {exc}", file=sys.stderr)
        try:
            remove_worktree(repo, after_tree)
        except Exception as exc:
            print(f"[warn] remove worktree after failed: {exc}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
