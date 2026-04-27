#!/usr/bin/env python3
import argparse
import math
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


CUSTOM_TESTS: list[TestCase] = [
    TestCase("21-create-many", ["100"]),
    TestCase("22-create-many-recursive", ["100"]),
    TestCase("23-create-many-once", ["100"]),
    TestCase("31-switch-many", ["100", "100"]),
    TestCase("32-switch-many-join", ["100", "100"]),
    TestCase("33-switch-many-cascade", ["100", "100"]),
    TestCase("51-fibonacci", ["18"]),
    TestCase("61-mutex", ["40"]),
    TestCase("62-mutex", ["10"]),
    TestCase("63-mutex-equity", []),
    TestCase("64-mutex-join", []),
    TestCase("sum", []),
    TestCase("sort", []),
    TestCase("reduction", []),
    TestCase("matrix_mul", []),
]


def run_cmd(cmd: list[str], cwd: Path) -> None:
    result = subprocess.run(cmd, cwd=str(cwd), check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Commande echouee ({result.returncode}): {' '.join(cmd)}")


def format_run_error(proc: subprocess.CompletedProcess[str]) -> str:
    return (
        f"code={proc.returncode}; "
        f"stdout={proc.stdout[-200:].strip()}; "
        f"stderr={proc.stderr[-200:].strip()}"
    )


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


def default_thread_counts() -> list[int]:
    return [10, 50, 100, 200]


def parse_thread_counts(value: Optional[str]) -> List[int]:
    if not value:
        return default_thread_counts()
    thread_counts = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        thread_count = int(item)
        if thread_count < 1:
            raise ValueError("Chaque valeur de --threads doit etre >= 1")
        thread_counts.append(thread_count)
    if not thread_counts:
        raise ValueError("--threads ne contient aucune valeur valide")
    return sorted(set(thread_counts))


def parse_fibonacci_values(start: int, stop: int, step: int) -> list[int]:
    if start < 1:
        raise ValueError("--fibonacci-start doit etre >= 1")
    if stop < start:
        raise ValueError("--fibonacci-max doit etre >= --fibonacci-start")
    if step < 1:
        raise ValueError("--fibonacci-step doit etre >= 1")
    return list(range(start, stop + 1, step))


def resolve_png_output_path(repo: Path, png_arg: str) -> Path:
    output = Path(png_arg)
    if output.is_absolute():
        return output

    # Tout chemin relatif est normalise vers graph_exec_time_comparison/bench.
    parts = output.parts
    if parts and parts[0] == "bench":
        output = Path(*parts[1:]) if len(parts) > 1 else Path("results.png")
    return (repo / "graph_exec_time_comparison" / "bench" / output).resolve()


def make_axes(test_count: int):
    if test_count == 1:
        fig, axes = plt.subplots(1, 1, figsize=(8, 5), sharex=True)
        return fig, [axes]

    ncols = 2 if test_count <= 4 else 3
    nrows = math.ceil(test_count / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=(6 * ncols, 3.8 * nrows), sharex=True)
    if hasattr(axes, "ravel"):
        flat_axes = list(axes.ravel())
    else:
        flat_axes = [axes]
    return fig, flat_axes


def run_binary(
    exe: Path,
    args: list[str],
    repo: Path,
    env: dict[str, str],
    timeout_s: int,
    core_count: Optional[int] = None,
) -> tuple[bool, float, str]:
    return timed_run(
        [str(exe)] + args,
        cwd=repo,
        env=env,
        timeout_s=timeout_s,
        core_count=core_count,
    )


def supports_thread_scaling(test: TestCase) -> bool:
    return (test.name.startswith("2") or test.name.startswith("3")) and len(test.args) >= 1


def with_thread_count_args(test: TestCase, thread_count: int) -> list[str]:
    args = list(test.args)
    args[0] = str(thread_count)
    return args


def plot_comparison(
    path: Path,
    selected_tests: list[TestCase],
    x_values: list[int],
    custom_times: dict[str, dict[int, list[float]]],
    pthread_times: dict[str, dict[int, list[float]]],
    x_label: str,
    title: str,
) -> None:
    if not selected_tests:
        return

    path.parent.mkdir(parents=True, exist_ok=True)
    fig, axes = make_axes(len(selected_tests))

    for axis in axes[len(selected_tests):]:
        axis.axis("off")

    for axis, test in zip(axes, selected_tests):
        y_custom = [median_or_nan(custom_times[test.name][x]) for x in x_values]
        y_pthread = [median_or_nan(pthread_times[test.name][x]) for x in x_values]

        axis.plot(x_values, y_custom, marker="o", linewidth=2, label="thread.c")
        axis.plot(x_values, y_pthread, marker="o", linewidth=2, label="pthread")
        axis.set_title(test.name)
        axis.set_xlabel(x_label)
        axis.set_ylabel("Temps median (s)")
        axis.grid(True, axis="y", alpha=0.3)
        axis.set_xticks(x_values)
        axis.legend()

    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def get_affinity_prefix(core_count: int) -> Tuple[List[str], Optional[Callable[[], None]]]:
    if hasattr(os, "sched_setaffinity"):
        core_set = set(range(core_count))

        def set_affinity() -> None:
            os.sched_setaffinity(0, core_set)

        return [], set_affinity

    if shutil.which("taskset"):
        return ["taskset", "-c", f"0-{core_count - 1}"], None

    return [], None


def timed_run(
    cmd: list[str],
    cwd: Path,
    env: dict[str, str],
    timeout_s: int,
    core_count: Optional[int] = None,
) -> tuple[bool, float, str]:
    prefix: list[str] = []
    preexec: Optional[Callable[[], None]] = None
    if core_count is not None:
        prefix, preexec = get_affinity_prefix(core_count)

    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            prefix + cmd,
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
        if proc.returncode == 0:
            return True, dt, ""
        return False, dt, format_run_error(proc)
    except subprocess.TimeoutExpired:
        dt = time.perf_counter() - t0
        return False, dt, f"timeout>{timeout_s}s"


def run_sweep(
    tests: list[TestCase],
    values: list[int],
    runs: int,
    total: int,
    done_start: int,
    repo: Path,
    bin_dir: Path,
    env: dict[str, str],
    timeout_s: int,
    custom_store: dict[str, dict[int, list[float]]],
    pthread_store: dict[str, dict[int, list[float]]],
    args_for_value: Callable[[TestCase, int], list[str]],
    core_for_value: Callable[[int], Optional[int]],
    progress_for_value: Callable[[TestCase, str, int], str],
    error_for_value: Callable[[TestCase, str, int, str], str],
) -> tuple[int, int]:
    done = done_start
    success_count = 0

    for test in tests:
        for value in values:
            test_args = args_for_value(test, value)
            core_count = core_for_value(value)
            for _ in range(1, runs + 1):
                for impl, exe in [
                    ("custom", bin_dir / test.name),
                    ("pthread", bin_dir / f"{test.name}-pthread"),
                ]:
                    done += 1
                    print(f"[{done}/{total}] {progress_for_value(test, impl, value)}", flush=True)
                    ok, dt, detail = run_binary(
                        exe,
                        test_args,
                        repo=repo,
                        env=env,
                        timeout_s=timeout_s,
                        core_count=core_count,
                    )
                    if ok:
                        success_count += 1
                        target = custom_store if impl == "custom" else pthread_store
                        target[test.name][value].append(dt)
                    else:
                        print(error_for_value(test, impl, value, detail), flush=True)

    return done, success_count


def run_progressive_sweep(
    test: TestCase,
    values: list[int],
    runs: int,
    total: int,
    done_start: int,
    repo: Path,
    bin_dir: Path,
    env: dict[str, str],
    timeout_s: int,
    custom_store: dict[str, dict[int, list[float]]],
    pthread_store: dict[str, dict[int, list[float]]],
    args_for_value: Callable[[int], list[str]],
    progress_for_value: Callable[[str, int], str],
    error_for_value: Callable[[str, int, str], str],
) -> tuple[int, int]:
    done = done_start
    success_count = 0
    active_impls = {"custom": True, "pthread": True}

    for value in values:
        test_args = args_for_value(value)
        for _ in range(1, runs + 1):
            for impl, exe in [
                ("custom", bin_dir / test.name),
                ("pthread", bin_dir / f"{test.name}-pthread"),
            ]:
                if not active_impls[impl]:
                    continue

                done += 1
                print(f"[{done}/{total}] {progress_for_value(impl, value)}", flush=True)
                ok, dt, detail = run_binary(
                    exe,
                    test_args,
                    repo=repo,
                    env=env,
                    timeout_s=timeout_s,
                )
                if ok:
                    success_count += 1
                    target = custom_store if impl == "custom" else pthread_store
                    target[test.name][value].append(dt)
                else:
                    active_impls[impl] = False
                    print(error_for_value(impl, value, detail), flush=True)

    return done, success_count


def plot_scaling(
    path: Path,
    selected_tests: list[TestCase],
    core_counts: list[int],
    custom_times: dict[str, dict[int, list[float]]],
    pthread_times: dict[str, dict[int, list[float]]],
) -> None:
    plot_comparison(
        path,
        selected_tests,
        core_counts,
        custom_times,
        pthread_times,
        x_label="Nombre de coeurs utilises",
        title="Temps d'execution en fonction du nombre de coeurs",
    )


def plot_thread_scaling(
    path: Path,
    selected_tests: list[TestCase],
    thread_counts: list[int],
    custom_times: dict[str, dict[int, list[float]]],
    pthread_times: dict[str, dict[int, list[float]]],
    fixed_core_count: int,
) -> None:
    plot_comparison(
        path,
        selected_tests,
        thread_counts,
        custom_times,
        pthread_times,
        x_label="Nombre de threads",
        title=f"Temps d'execution en fonction du nombre de threads (coeurs fixes={fixed_core_count})",
    )


def plot_fibonacci(
    path: Path,
    selected_tests: list[TestCase],
    fibonacci_values: list[int],
    custom_times: dict[str, dict[int, list[float]]],
    pthread_times: dict[str, dict[int, list[float]]],
) -> None:
    plot_comparison(
        path,
        selected_tests,
        fibonacci_values,
        custom_times,
        pthread_times,
        x_label="Valeur de fibonacci",
        title="Temps d'execution en fonction de la valeur de fibonacci",
    )


def main() -> int:

    parser = argparse.ArgumentParser(description="Benchmark simple maison vs pthread")
    parser.add_argument("--repo", default=".")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=20, help="Timeout par test en secondes")
    parser.add_argument("--png", default="graph_exec_time_comparison/bench/results.png")
    parser.add_argument("--cores", default=None, help="Liste de coeurs limites, ex: 1,2,4,8")
    parser.add_argument("--threads", default=None, help="Liste de nombres de threads pour tests 20/30, ex: 10,50,100")
    parser.add_argument("--fibonacci-start", type=int, default=1, help="Valeur de depart pour le balayage fibonacci")
    parser.add_argument("--fibonacci-max", type=int, default=45, help="Valeur maximale pour le balayage fibonacci")
    parser.add_argument("--fibonacci-step", type=int, default=1, help="Pas pour le balayage fibonacci")
    parser.add_argument(
        "--test",
        default=None,
        choices=[test.name for test in CUSTOM_TESTS],
        help="N'exécute qu'un seul test custom",
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument(
        "--custom-tests",
        action="store_true",
        help="N'exécute que les benchmarks custom avec un graphe temps vs coeurs",
    )
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
        if args.test is not None:
            # Chercher le test dans CUSTOM_TESTS pour préserver ses args
            selected_tests = [t for t in CUSTOM_TESTS if t.name == args.test]
            if not selected_tests:
                print(f"Test '{args.test}' not found in CUSTOM_TESTS")
                return 2
        else:
            selected_tests = CUSTOM_TESTS
    else:
        print("Ce script ne gère plus le mode comparaison globale; utilise --custom-tests.")
        return 2

    try:
        core_counts = parse_core_counts(args.cores) if args.custom_tests else [1]
        thread_counts = parse_thread_counts(args.threads) if args.custom_tests else [1]
        fibonacci_values = parse_fibonacci_values(args.fibonacci_start, args.fibonacci_max, args.fibonacci_step)
    except ValueError as exc:
        print(str(exc))
        return 2

    fibonacci_test = next((test for test in selected_tests if test.name == "51-fibonacci"), None)
    if len(selected_tests) == 1 and fibonacci_test is not None:
        custom_times: dict[str, dict[int, list[float]]] = {
            fibonacci_test.name: {value: [] for value in fibonacci_values}
        }
        pthread_times: dict[str, dict[int, list[float]]] = {
            fibonacci_test.name: {value: [] for value in fibonacci_values}
        }

        total = len(fibonacci_values) * args.runs * 2
        done, success_count = run_progressive_sweep(
            test=fibonacci_test,
            values=fibonacci_values,
            runs=args.runs,
            total=total,
            done_start=0,
            repo=repo,
            bin_dir=bin_dir,
            env=env,
            timeout_s=args.timeout,
            custom_store=custom_times,
            pthread_store=pthread_times,
            args_for_value=lambda value: [str(value)],
            progress_for_value=lambda impl, value: f"{fibonacci_test.name} {impl} fib={value}",
            error_for_value=lambda impl, value, detail: (
                f"  -> echec ({impl} {fibonacci_test.name}, fib={value}): {detail}"
            ),
        )

        png_path = resolve_png_output_path(repo, args.png)
        plot_fibonacci(png_path, selected_tests, fibonacci_values, custom_times, pthread_times)

        print(f"Graphe : {png_path}")
        print(f"Executions reussies: {success_count}/{total}")
        return 0

    custom_times: dict[str, dict[int, list[float]]] = {
        t.name: {core: [] for core in core_counts} for t in selected_tests
    }
    pthread_times: dict[str, dict[int, list[float]]] = {
        t.name: {core: [] for core in core_counts} for t in selected_tests
    }

    thread_scaling_tests = [test for test in selected_tests if supports_thread_scaling(test)]
    custom_thread_times: dict[str, dict[int, list[float]]] = {
        t.name: {thread_count: [] for thread_count in thread_counts} for t in thread_scaling_tests
    }
    pthread_thread_times: dict[str, dict[int, list[float]]] = {
        t.name: {thread_count: [] for thread_count in thread_counts} for t in thread_scaling_tests
    }

    fixed_core_for_thread_scaling = max(core_counts)

    total_core_runs = len(selected_tests) * args.runs * 2 * len(core_counts)
    total_thread_runs = len(thread_scaling_tests) * args.runs * 2 * len(thread_counts)
    total = total_core_runs + total_thread_runs
    done, success_count = run_sweep(
        tests=selected_tests,
        values=core_counts,
        runs=args.runs,
        total=total,
        done_start=0,
        repo=repo,
        bin_dir=bin_dir,
        env=env,
        timeout_s=args.timeout,
        custom_store=custom_times,
        pthread_store=pthread_times,
        args_for_value=lambda test, _core: test.args,
        core_for_value=lambda core: core,
        progress_for_value=lambda test, impl, core: f"{test.name} {impl} cores={core}",
        error_for_value=lambda test, impl, core, detail: f"  -> echec ({impl} {test.name}, cores={core}): {detail}",
    )

    done, thread_success_count = run_sweep(
        tests=thread_scaling_tests,
        values=thread_counts,
        runs=args.runs,
        total=total,
        done_start=done,
        repo=repo,
        bin_dir=bin_dir,
        env=env,
        timeout_s=args.timeout,
        custom_store=custom_thread_times,
        pthread_store=pthread_thread_times,
        args_for_value=with_thread_count_args,
        core_for_value=lambda _thread_count: fixed_core_for_thread_scaling,
        progress_for_value=lambda test, impl, thread_count: (
            f"{test.name} {impl} threads={thread_count} cores={fixed_core_for_thread_scaling}"
        ),
        error_for_value=lambda test, impl, thread_count, detail: (
            f"  -> echec ({impl} {test.name}, threads={thread_count}, cores={fixed_core_for_thread_scaling}): {detail}"
        ),
    )
    success_count += thread_success_count

    png_path = resolve_png_output_path(repo, args.png)
    png_thread_path = png_path.with_name(f"{png_path.stem}_threads{png_path.suffix}")

    plot_scaling(png_path, selected_tests, core_counts, custom_times, pthread_times)
    plot_thread_scaling(
        png_thread_path,
        thread_scaling_tests,
        thread_counts,
        custom_thread_times,
        pthread_thread_times,
        fixed_core_for_thread_scaling,
    )

    print(f"Graphe : {png_path}")
    if thread_scaling_tests:
        print(f"Graphe threads (tests 20/30): {png_thread_path}")
    print(f"Executions reussies: {success_count}/{total}")
    return 0

#commande pour compiler pour un test exemple du test 33 : make graphs ARGS="--custom-tests --test 33-switch-many-cascade --png graph_exec_time_comparison/bench/33.png --cores 1,2,4,8 --threads 10,50,100,200 --runs 3"
if __name__ == "__main__":
    raise SystemExit(main())


