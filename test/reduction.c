#include "thread.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define ARRAY_SIZE 10000000 // Size of the array
#define BLOCK_SIZE 100000   // Minimum size of subarray to create new threads

typedef enum { REDUCE_SUM, REDUCE_MIN, REDUCE_MAX } ReduceOp;

// Structure to pass arguments to each thread
typedef struct {
  int *array;       // Pointer to the array
  int start;        // Start index
  int end;          // End index
  ReduceOp op;      // Reduction operation
  long long result; // Result of the reduction for this segment
} ReduceArgs;

// Function to apply the reduction operation
long long apply_op(long long a, long long b, ReduceOp op) {
  switch (op) {
    case REDUCE_SUM:
      return a + b;
    case REDUCE_MIN:
      return a < b ? a : b;
    case REDUCE_MAX:
      return a > b ? a : b;
    default:
      return 0;
  }
}

// Thread function to compute reduction on a segment
void *reduce_thread(void *arg) {
  ReduceArgs *args = (ReduceArgs *)arg;
  int start = args->start;
  int end = args->end;
  ReduceOp op = args->op;

  // If the segment is small enough, compute directly
  if (end - start <= BLOCK_SIZE) {
    long long res = args->array[start];
    for (int i = start + 1; i < end; ++i) {
      res = apply_op(res, args->array[i], op);
    }
    args->result = res;
    return NULL;
  }

  // Otherwise, split and create two threads
  int mid = start + (end - start) / 2;
  ReduceArgs left = {args->array, start, mid, op, 0};
  ReduceArgs right = {args->array, mid, end, op, 0};
  thread_t t_left, t_right;

  thread_create(&t_left, reduce_thread, &left);
  thread_create(&t_right, reduce_thread, &right);
  thread_join(t_left, NULL);
  thread_join(t_right, NULL);

  args->result = apply_op(left.result, right.result, op);
  return NULL;
}

int main() {
#ifdef THREAD_MULTICORE
  thread_set_concurrency(4);
#endif
  int *array = malloc(sizeof(int) * ARRAY_SIZE);
  assert(array != NULL);
  for (int i = 0; i < ARRAY_SIZE; ++i) {
    array[i] = rand() % 1000 + 1;
  }

  ReduceOp ops[] = {REDUCE_SUM, REDUCE_MIN, REDUCE_MAX};
  const char *op_names[] = {"sum", "min", "max"};

  for (int op_idx = 0; op_idx < 3; ++op_idx) {
    ReduceArgs args = {array, 0, ARRAY_SIZE, ops[op_idx], 0};
    thread_t t;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    thread_create(&t, reduce_thread, &args);
    thread_join(t, NULL);
    gettimeofday(&end, NULL);
    long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("%s = %lld\n", op_names[op_idx], args.result);
    if (elapsed_us < 1000)
      printf("Execution time for %s: %lld us\n", op_names[op_idx], elapsed_us);
    else if (elapsed_us < 1000000)
      printf("Execution time for %s: %.3f ms\n", op_names[op_idx], elapsed_us / 1000.0);
    else
      printf("Execution time for %s: %.3f s\n", op_names[op_idx], elapsed_us / 1000000.0);
  }

  free(array);
  return 0;
}
