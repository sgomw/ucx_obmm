/*
 * mpi_correctness.c -- 2-rank correctness check on payload bytes.
 *
 * Rank 0 fills a buffer with a deterministic pattern keyed by iteration,
 * sends it; rank 1 verifies every byte. Any mismatch prints offending
 * offset + expected/actual and aborts.
 *
 * Useful for catching truncation, byte-order bugs, generation drift,
 * stale-slot reuse, etc.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ITERS  500

static const int sizes[] = {1, 7, 8, 9, 64, 255, 256, 1024, 1900};
static const int nsizes  = sizeof(sizes) / sizeof(sizes[0]);

static uint8_t pat(int iter, int off) {
    return (uint8_t)((iter * 1315423911u) ^ (off * 2654435761u));
}

int main(int argc, char **argv) {
    int rank, size, i, k, off;
    char *buf;
    int max_size = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        if (rank == 0) {
            fprintf(stderr, "mpi_correctness needs exactly 2 ranks (got %d)\n",
                    size);
        }
        MPI_Finalize();
        return 1;
    }

    for (i = 0; i < nsizes; i++) {
        if (sizes[i] > max_size) max_size = sizes[i];
    }
    buf = (char *)malloc(max_size);
    if (!buf) { MPI_Abort(MPI_COMM_WORLD, 1); }

    if (rank == 0) {
        printf("%-10s %-10s %-10s\n", "bytes", "iters", "result");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n = sizes[i];
        long mismatches = 0;

        for (k = 0; k < ITERS; k++) {
            if (rank == 0) {
                for (off = 0; off < n; off++) {
                    buf[off] = (char)pat(k, off);
                }
                MPI_Send(buf, n, MPI_BYTE, 1, 0, MPI_COMM_WORLD);
            } else {
                memset(buf, 0, n);
                MPI_Recv(buf, n, MPI_BYTE, 0, 0, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
                for (off = 0; off < n; off++) {
                    uint8_t want = pat(k, off);
                    if ((uint8_t)buf[off] != want) {
                        if (mismatches < 5) {
                            fprintf(stderr,
                                    "MISMATCH iter=%d off=%d got=0x%02x want=0x%02x\n",
                                    k, off, (uint8_t)buf[off], want);
                        }
                        mismatches++;
                    }
                }
            }
        }

        long total = 0;
        MPI_Reduce(&mismatches, &total, 1, MPI_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-10d %-10d %s (%ld bad bytes)\n",
                   n, ITERS, total == 0 ? "OK" : "FAIL", total);
            fflush(stdout);
        }
        if (total != 0) {
            MPI_Abort(MPI_COMM_WORLD, 2);
        }
    }

    free(buf);
    MPI_Finalize();
    return 0;
}