/*
 * mpi_sanity.c -- minimal MPI hello world.
 * Confirms MPI initializes and ranks can print over MPI.
 *
 * Expected: each rank prints one line, then "ALL DONE" from rank 0.
 */
#include <mpi.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int rank, size;
    char host[256] = {0};

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    gethostname(host, sizeof(host) - 1);

    printf("rank %d/%d host=%s pid=%d\n", rank, size, host, (int)getpid());
    fflush(stdout);

    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
        printf("ALL DONE size=%d\n", size);
        fflush(stdout);
    }

    MPI_Finalize();
    return 0;
}