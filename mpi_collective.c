/*
 * mpi_collective.c -- small collectives over obmm (Bcast, Allreduce, Barrier).
 *
 * 2 ranks. Buffers small enough to stay under am_short.
 * Verifies the collectives produce expected results.
 */
#include <mpi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int rank, size;
    int buf[64];
    int sum[64];
    int i;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (rank == 0) {
        printf("ranks=%d\n", size);
        fflush(stdout);
    }

    /* Barrier */
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) { printf("Barrier: OK\n"); fflush(stdout); }

    /* Bcast */
    for (i = 0; i < 64; i++) buf[i] = (rank == 0) ? (i + 100) : 0;
    MPI_Bcast(buf, 64, MPI_INT, 0, MPI_COMM_WORLD);
    int bcast_ok = 1;
    for (i = 0; i < 64; i++) {
        if (buf[i] != i + 100) { bcast_ok = 0; break; }
    }
    int all_bcast_ok = 0;
    MPI_Reduce(&bcast_ok, &all_bcast_ok, 1, MPI_INT, MPI_LAND, 0,
               MPI_COMM_WORLD);
    if (rank == 0) {
        printf("Bcast: %s\n", all_bcast_ok ? "OK" : "FAIL");
        fflush(stdout);
    }

    /* Allreduce */
    for (i = 0; i < 64; i++) buf[i] = rank + 1;  /* 1, 2, ... */
    MPI_Allreduce(buf, sum, 64, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    int expected = (size * (size + 1)) / 2;
    int allred_ok = 1;
    for (i = 0; i < 64; i++) {
        if (sum[i] != expected) { allred_ok = 0; break; }
    }
    int all_allred_ok = 0;
    MPI_Reduce(&allred_ok, &all_allred_ok, 1, MPI_INT, MPI_LAND, 0,
               MPI_COMM_WORLD);
    if (rank == 0) {
        printf("Allreduce: %s (expected %d)\n",
               all_allred_ok ? "OK" : "FAIL", expected);
        fflush(stdout);
    }

    MPI_Finalize();
    return 0;
}