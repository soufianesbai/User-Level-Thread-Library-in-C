#!/usr/bin/env bash

set -u

ITERATIONS="${1:-20}"

if ! [[ "$ITERATIONS" =~ ^[0-9]+$ ]] || [ "$ITERATIONS" -le 0 ]; then
  echo "Usage: $0 [iterations>0]"
  exit 2
fi

echo "[build] make MULTICORE=1 tests"
if ! make MULTICORE=1 tests >/dev/null; then
  echo "Build failed"
  exit 1
fi

tests=(
  bin/test_multicore_cpu
  bin/test_multicore_yield
  bin/test_multicore_create_join
  bin/test_multicore_mutex
  bin/test_multicore_mutex_stress
  bin/test_multicore_semaphore
)

failed=0

for t in "${tests[@]}"; do
  if [ ! -x "$t" ]; then
    echo "[missing] $t"
    failed=$((failed + 1))
    continue
  fi

  ok=1
  for i in $(seq 1 "$ITERATIONS"); do
    "$t" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
      ok=0
      echo "[FAIL] $t at iteration $i (rc=$rc)"
      break
    fi
  done

  if [ "$ok" -eq 1 ]; then
    echo "[PASS] $t x$ITERATIONS"
  else
    failed=$((failed + 1))
  fi
done

if [ "$failed" -ne 0 ]; then
  echo "Multicore stress summary: $failed failing test(s)."
  exit 1
fi

echo "Multicore stress summary: all tests passed."
exit 0
