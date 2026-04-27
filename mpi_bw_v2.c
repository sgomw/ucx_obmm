/*
 * mpi_bw_v2.c -- 2-rank one-way bandwidth across short / bcopy / fragmented.
 *
 * Same window-of-Isend pattern as v1, but sweep extends past 2032 to
 * exercise am_bcopy, and into 1 MiB to exercise UCP fragmentation.
 *
 * Note: the WINDOW size + max message means total in-flight bytes can
 * reach WINDOW*max_size; we keep WINDOW=32 and max=1MB so the buffer is
 * 32 MiB per rank, comfortably under the 128 MiB obmm region (which is
 * a different budget anyway -- the buffer is process-local).
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW 32
#define WARMUP 50

static const int sizes[] = {
    64, 256, 1024, 1900,
    /* boundaries */
    2032, 2033, 4096, 4097,
    /* fragmented */
    8192, 16384, 65536, 262144, 1048576
};
/* Iters per size: more iters at small sizes to get stable BW; fewer at
 * large sizes to keep total runtime reasonable. */
static int iters_for(int n) {
    if (n <= 1024)  return 4000;
    if (n <= 8192)  return 1500;
    if (n <= 65536) return 500;
    return 100;
}
static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

int main(int argc, char **argv) {
    int rank, size, i, k, w;
    char *buf;
    int max_size = 0;
    MPI_Request reqs[WINDOW];

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        if (rank == 0) {
            fprintf(stderr, "mpi_bw_v2 needs exactly 2 ranks (got %d)\n",
                    size);
        }
        MPI_Finalize();
        return 1;
    }

    for (i = 0; i < nsizes; i++) {
        if (sizes[i] > max_size) max_size = sizes[i];
    }
    buf = (char *)malloc((size_t)max_size * WINDOW);
    if (!buf) { MPI_Abort(MPI_COMM_WORLD, 1); }
    memset(buf, (rank == 0) ? 0xAA : 0x55, (size_t)max_size * WINDOW);

    if (rank == 0) {
        printf("%-10s %-10s %-10s %-12s\n",
               "bytes", "window", "iters", "MB/s");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n     = sizes[i];
        int iters = iters_for(n);
        char ack  = 0;

        for (k = 0; k < WARMUP; k++) {
            if (rank == 0) {
                for (w = 0; w < WINDOW; w++) {
                    MPI_Isend(buf + (size_t)w * n, n, MPI_BYTE, 1, 0,
                              MPI_COMM_WORLD, &reqs[w]);
                }
                MPI_Waitall(WINDOW, reqs, MPI_STATUSES_IGNORE);
                MPI_Recv(&ack, 1, MPI_BYTE, 1, 1, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
            } else {
                for (w = 0; w < WINDOW; w++) {
                    MPI_Irecv(buf + (size_t)w * n, n, MPI_BYTE, 0, 0,
                              MPI_COMM_WORLD, &reqs[w]);
                }
                MPI_Waitall(WINDOW, reqs, MPI_STATUSES_IGNORE);
                MPI_Send(&ack, 1, MPI_BYTE, 0, 1, MPI_COMM_WORLD);
            }
        }

        MPI_Barrier(MPI_COMM_WORLD);
        double t0 = MPI_Wtime();
        for (k = 0; k < iters; k++) {
            if (rank == 0) {
                for (w = 0; w < WINDOW; w++) {
                    MPI_Isend(buf + (size_t)w * n, n, MPI_BYTE, 1, 0,
                              MPI_COMM_WORLD, &reqs[w]);
                }
                MPI_Waitall(WINDOW, reqs, MPI_STATUSES_IGNORE);
                MPI_Recv(&ack, 1, MPI_BYTE, 1, 1, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
            } else {
                for (w = 0; w < WINDOW; w++) {
                    MPI_Irecv(buf + (size_t)w * n, n, MPI_BYTE, 0, 0,
                              MPI_COMM_WORLD, &reqs[w]);
                }
                MPI_Waitall(WINDOW, reqs, MPI_STATUSES_IGNORE);
                MPI_Send(&ack, 1, MPI_BYTE, 0, 1, MPI_COMM_WORLD);
            }
        }
        double t1 = MPI_Wtime();

        if (rank == 0) {
            double total_bytes = (double)n * WINDOW * iters;
            double mbps = total_bytes / (t1 - t0) / 1.0e6;
            printf("%-10d %-10d %-10d %-12.2f\n",
                   n, WINDOW, iters, mbps);
            fflush(stdout);
        }
    }

    free(buf);
    MPI_Finalize();
    return 0;
}
