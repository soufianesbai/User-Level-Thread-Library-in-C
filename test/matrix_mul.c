#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <stdint.h>
#include "thread.h"

#define N 1000 // Matrix size (N x N)
#define BLOCK_SIZE 50 // Number of rows per thread

// Structure to pass arguments to each thread
typedef struct {
    int *A; // Pointer to matrix A
    int *B; // Pointer to matrix B
    int *C; // Pointer to result matrix C
    int row_start; // Starting row index for this thread
    int row_end; // Ending row index for this thread
    int n; // Size of the matrices
} MatMulArgs;

// Thread function to compute a block of rows of the result matrix
void *matrix_mul_thread(void *arg) {
    MatMulArgs *args = (MatMulArgs *)arg;
    int n = args->n;
    for (int i = args->row_start; i < args->row_end; ++i) {
        for (int j = 0; j < n; ++j) {
            int sum = 0;
            for (int k = 0; k < n; ++k) {
                sum += args->A[i * n + k] * args->B[k * n + j];
            }
            args->C[i * n + j] = sum;
        }
    }
    return NULL;
}

int main() {
    int *A = malloc(sizeof(int) * N * N);
    int *B = malloc(sizeof(int) * N * N);
    int *C = malloc(sizeof(int) * N * N);
    assert(A != NULL && B != NULL && C != NULL);

    // Initialize matrices A and B with random values
    for (int i = 0; i < N * N; ++i) {
        A[i] = rand() % 100;
        B[i] = rand() % 100;
    }
    
    int num_blocks = (N + BLOCK_SIZE - 1) / BLOCK_SIZE; // Calculate number of blocks needed, rounding up
    thread_t *threads = malloc(sizeof(thread_t) * num_blocks);
    MatMulArgs *args = malloc(sizeof(MatMulArgs) * num_blocks);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    // Create threads for each block of rows
    for (int b = 0; b < num_blocks; ++b) {
        int row_start = b * BLOCK_SIZE;
        int row_end = (b + 1) * BLOCK_SIZE;
        if (row_end > N) row_end = N;
        args[b].A = A;
        args[b].B = B;
        args[b].C = C;
        args[b].row_start = row_start;
        args[b].row_end = row_end;
        args[b].n = N;
        thread_create(&threads[b], matrix_mul_thread, &args[b]);
    }

    // Wait for all threads to finish
    for (int b = 0; b < num_blocks; ++b) {
        thread_join(threads[b], NULL);
    }

    gettimeofday(&end, NULL);
    long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("Matrix-matrix multiplication completed.\n");
    if (elapsed_us < 1000)
        printf("Execution time: %lld us\n", elapsed_us);
    else if (elapsed_us < 1000000)
        printf("Execution time: %.3f ms\n", elapsed_us / 1000.0);
    else
        printf("Execution time: %.3f s\n", elapsed_us / 1000000.0);

    free(A);
    free(B);
    free(C);
    free(threads);
    free(args);
    return 0;
}
