#!/bin/bash

set -e

tests=(sum sort reduction matrix_mul)

for t in "${tests[@]}"; do
  make graphs ARGS="--custom-tests --png graph_exec_time_comparison/bench/results_${t}.png" &
done

wait
echo "All benchmarks are finished. Results are in graph_exec_time_comparison/bench/results_*.png."
