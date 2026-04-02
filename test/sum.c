#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "thread.h"

#define ARRAY_SIZE 100000000 // Size of the array to sum
#define BLOCK_SIZE 100000    // Minimum size of subarray to create new threads

// Structure to pass arguments to each thread
typedef struct {
  int *array;       // Pointer to the array
  int start;        // Start index
  int end;          // End index
  long long result; // Result of the sum for this segment
} SumArgs;

// Thread function to compute the sum of a segment of the array
void *sum_thread(void *arg) {
  SumArgs *args = (SumArgs *)arg;
  int start = args->start;
  int end = args->end;

  // If the segment is small enough, sum directly
  if (end - start <= BLOCK_SIZE) {
    long long sum = 0;
    for (int i = start; i < end; ++i) {
      sum += args->array[i];
    }
    args->result = sum;
    return NULL;
  }

  // Otherwise, split the segment in two and create two threads
  int mid = start + (end - start) / 2;
  SumArgs left = {args->array, start, mid, 0};
  SumArgs right = {args->array, mid, end, 0};
  thread_t t_left, t_right;

  // Create threads for left and right halves
  thread_create(&t_left, sum_thread, &left);
  thread_create(&t_right, sum_thread, &right);

  // Wait for both threads to finish
  thread_join(t_left, NULL);
  thread_join(t_right, NULL);

  // Combine the results
  args->result = left.result + right.result;
  return NULL;
}

int main() {
  // Allocate and initialize the array
  int *array = malloc(sizeof(int) * ARRAY_SIZE);
  assert(array != NULL);
  for (int i = 0; i < ARRAY_SIZE; ++i) {
    array[i] = i + 1; // For testing: expected sum = ARRAY_SIZE * (ARRAY_SIZE + 1) / 2
  }

  // Prepare arguments for the initial thread
  SumArgs args = {array, 0, ARRAY_SIZE, 0};
  thread_t t;
  struct timeval start, end;

  // Measure execution time
  gettimeofday(&start, NULL);
  thread_create(&t, sum_thread, &args);
  thread_join(t, NULL);
  gettimeofday(&end, NULL);

  long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
  printf("Sum = %lld\n", args.result);
  if (elapsed_us < 1000)
    printf("Execution time: %lld us\n", elapsed_us);
  else if (elapsed_us < 1000000)
    printf("Execution time: %.3f ms\n", elapsed_us / 1000.0);
  else
    printf("Execution time: %.3f s\n", elapsed_us / 1000000.0);

  free(array);
  return 0;
}
