/*
 * mpi_bw.c -- 2-rank one-way bandwidth (window of non-blocking sends).
 *
 * Each iteration: rank 0 posts WINDOW Isends, rank 1 posts WINDOW Irecvs,
 * both Waitall, rank 1 sends one ack. Reports MB/s.
 *
 * Sizes capped under am_short capacity.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW    64
#define ITERS     2000
#define WARMUP    200

static const int sizes[] = {64, 256, 1024, 1900};
static const int nsizes  = sizeof(sizes) / sizeof(sizes[0]);

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
            fprintf(stderr, "mpi_bw needs exactly 2 ranks (got %d)\n", size);
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
        int n = sizes[i];
        char ack = 0;

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
        for (k = 0; k < ITERS; k++) {
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
            double total_bytes = (double)n * WINDOW * ITERS;
            double mbps = total_bytes / (t1 - t0) / 1.0e6;
            printf("%-10d %-10d %-10d %-12.2f\n",
                   n, WINDOW, ITERS, mbps);
            fflush(stdout);
        }
    }

    free(buf);
    MPI_Finalize();
    return 0;
}