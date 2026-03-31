#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <stdint.h>

#include "thread.h"

#define ARRAY_SIZE 1000000 // Size of the array to sort
#define BLOCK_SIZE 10000 // Minimum size of subarray to create new threads

// Structure to pass arguments to each thread
typedef struct {
    int *array; // Pointer to the array
    int *temp; // Temporary array for merging
    int start; // Start index
    int end; // End index
} SortArgs;

// Merge two sorted subarrays array[start:mid] and array[mid:end] into temp
void merge(int *array, int *temp, int start, int mid, int end) {
    int i = start, j = mid, k = start;

    // Merge the two subarrays into temp
    while (i < mid && j < end) {
        if (array[i] < array[j]) temp[k++] = array[i++];
        else temp[k++] = array[j++];
    }

    // Copy any remaining elements
    while (i < mid) temp[k++] = array[i++];
    while (j < end) temp[k++] = array[j++];

    // Copy back to the original array
    for (i = start; i < end; ++i) array[i] = temp[i];
}

// Thread function to sort a segment of the array
void *mergesort_thread(void *arg) {
    SortArgs *args = (SortArgs *)arg;
    int start = args->start;
    int end = args->end;
    int *array = args->array;
    int *temp = args->temp;

    // If the segment is small enough, use insertion sort
    if (end - start <= BLOCK_SIZE) {
        for (int i = start + 1; i < end; ++i) {
            int key = array[i];
            int j = i - 1;
            while (j >= start && array[j] > key) {
                array[j + 1] = array[j];
                --j;
            }
            array[j + 1] = key;
        }
        return NULL;
    }

    // Otherwise, split the segment in two and create two threads
    int mid = start + (end - start) / 2;
    SortArgs left = {array, temp, start, mid};
    SortArgs right = {array, temp, mid, end};
    thread_t t_left, t_right;

    // Create threads for left and right halves
    thread_create(&t_left, mergesort_thread, &left);
    thread_create(&t_right, mergesort_thread, &right);

    // Wait for both threads to finish
    thread_join(t_left, NULL);
    thread_join(t_right, NULL);

    // Merge the sorted halves
    merge(array, temp, start, mid, end);
    return NULL;
}

int main() {
    // Allocate and initialize the array with random values
    int *array = malloc(sizeof(int) * ARRAY_SIZE);
    int *temp = malloc(sizeof(int) * ARRAY_SIZE);
    assert(array != NULL && temp != NULL);
    for (int i = 0; i < ARRAY_SIZE; ++i) {
        array[i] = rand();
    }

    // Prepare arguments for the initial thread
    SortArgs args = {array, temp, 0, ARRAY_SIZE};
    thread_t t;
    struct timeval start, end;

    // Measure execution time
    gettimeofday(&start, NULL);
    thread_create(&t, mergesort_thread, &args);
    thread_join(t, NULL);
    gettimeofday(&end, NULL);

    // Check if the array is sorted
    int sorted = 1;
    for (int i = 1; i < ARRAY_SIZE; ++i) {
        if (array[i-1] > array[i]) {
            sorted = 0;
            break;
        }
    }

    long long elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("Sorted: %s\n", sorted ? "yes" : "no");
    printf("Execution time: %lld s\n", elapsed);

    free(array);
    free(temp);
    return 0;
}
