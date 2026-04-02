#!/bin/bash

set -e

tests=(sum sort reduction matrix_mul)

for t in "${tests[@]}"; do
  make graphs ARGS="--custom-tests --csv bench/results_${t}.csv --png bench/results_${t}.png" &
done

wait
echo "All benchmarks are finished. Results are in bench/results_*.csv and bench/results_*.png."
