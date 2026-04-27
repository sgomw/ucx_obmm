/*
 * mpi_pingpong_v2.c -- 2-rank latency across am_short / am_bcopy /
 * fragmented-bcopy ranges.
 *
 * v1 (mpi_pingpong.c) capped at am_short. v2 sweeps the same boundaries
 * as mpi_correctness_v2 so we can read off the latency step at each
 * protocol transition and confirm bcopy is actually being exercised.
 *
 * Reports half-RTT (one-way latency) in microseconds.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARMUP 200
#define ITERS  2000

static const int sizes[] = {
    1, 8, 64, 256, 1024,
    2031, 2032, 2033,
    3000,
    4095, 4096, 4097,
    8192, 16384, 65536, 262144, 1048576
};
static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

int main(int argc, char **argv) {
    int rank, size, i, k;
    char *buf;
    int max_size = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        if (rank == 0) {
            fprintf(stderr,
                    "mpi_pingpong_v2 needs exactly 2 ranks (got %d)\n",
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
    memset(buf, (rank == 0) ? 0xAA : 0x55, max_size);

    if (rank == 0) {
        printf("%-10s %-10s %-12s\n", "bytes", "iters", "lat_us");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n = sizes[i];

        for (k = 0; k < WARMUP; k++) {
            if (rank == 0) {
                MPI_Send(buf, n, MPI_BYTE, 1, 0, MPI_COMM_WORLD);
                MPI_Recv(buf, n, MPI_BYTE, 1, 0, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
            } else {
                MPI_Recv(buf, n, MPI_BYTE, 0, 0, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
                MPI_Send(buf, n, MPI_BYTE, 0, 0, MPI_COMM_WORLD);
            }
        }

        MPI_Barrier(MPI_COMM_WORLD);
        double t0 = MPI_Wtime();
        for (k = 0; k < ITERS; k++) {
            if (rank == 0) {
                MPI_Send(buf, n, MPI_BYTE, 1, 0, MPI_COMM_WORLD);
                MPI_Recv(buf, n, MPI_BYTE, 1, 0, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
            } else {
                MPI_Recv(buf, n, MPI_BYTE, 0, 0, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
                MPI_Send(buf, n, MPI_BYTE, 0, 0, MPI_COMM_WORLD);
            }
        }
        double t1 = MPI_Wtime();

        if (rank == 0) {
            double lat_us = (t1 - t0) * 1.0e6 / (2.0 * ITERS);
            printf("%-10d %-10d %-12.3f\n", n, ITERS, lat_us);
            fflush(stdout);
        }
    }

    free(buf);
    MPI_Finalize();
    return 0;
}
