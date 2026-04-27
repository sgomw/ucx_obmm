/*
 * mpi_multi_v2.c -- N-rank correctness/load test for the obmm transport.
 *
 * Two phases per size:
 *   1) RING:    each rank r sends to (r+1)%N and recvs from (r-1+N)%N
 *               -- minimal connectivity, exercises every directed edge
 *               of the ring, both intra- and inter-node.
 *   2) A2A:     each rank does MPI_Alltoall -- N*(N-1) point-to-point
 *               flows, exercises every (sender, receiver) pair and
 *               max-fanout slot pressure.
 *
 * Per-(iter, size, src, dst) deterministic byte pattern; mismatches print
 * first 5 then abort.
 *
 * Use to validate np > 2 (e.g. np=8, 16, 32, 39, 40) on the obmm transport.
 *
 * Sizes span am_short -> am_bcopy -> UCP-fragmented bcopy.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ITERS  100

static const int sizes[] = {
    1, 64, 1024,
    2032, 2033,           /* short boundary */
    4096, 4097,           /* bcopy boundary */
    16384, 65536          /* fragmented */
};
static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

static uint8_t pat(int iter, int sz, int src, int dst, int off) {
    uint32_t h = (uint32_t)iter * 1315423911u
               ^ (uint32_t)sz   * 374761393u
               ^ (uint32_t)src  * 2246822519u
               ^ (uint32_t)dst  * 3266489917u
               ^ (uint32_t)off  * 2654435761u;
    return (uint8_t)(h ^ (h >> 16));
}

static long ring_phase(int rank, int N, char *sbuf, char *rbuf, int n) {
    int dst = (rank + 1) % N;
    int src = (rank - 1 + N) % N;
    long mism = 0;
    int  k, off;
    MPI_Request reqs[2];

    for (k = 0; k < ITERS; k++) {
        for (off = 0; off < n; off++) {
            sbuf[off] = (char)pat(k, n, rank, dst, off);
        }
        memset(rbuf, 0xCD, n);

        MPI_Irecv(rbuf, n, MPI_BYTE, src, 100, MPI_COMM_WORLD, &reqs[0]);
        MPI_Isend(sbuf, n, MPI_BYTE, dst,  100, MPI_COMM_WORLD, &reqs[1]);
        MPI_Waitall(2, reqs, MPI_STATUSES_IGNORE);

        for (off = 0; off < n; off++) {
            uint8_t want = pat(k, n, src, rank, off);
            if ((uint8_t)rbuf[off] != want) {
                if (mism < 5) {
                    fprintf(stderr,
                            "RING MISMATCH rank=%d size=%d iter=%d off=%d "
                            "got=0x%02x want=0x%02x src=%d\n",
                            rank, n, k, off, (uint8_t)rbuf[off], want, src);
                }
                mism++;
            }
        }
    }
    return mism;
}

static long a2a_phase(int rank, int N, char *sbuf, char *rbuf, int n) {
    long mism = 0;
    int  k, p, off;

    for (k = 0; k < ITERS; k++) {
        /* Pack send slot for each peer p with deterministic pattern. */
        for (p = 0; p < N; p++) {
            char *s = sbuf + (size_t)p * n;
            for (off = 0; off < n; off++) {
                s[off] = (char)pat(k, n, rank, p, off);
            }
        }
        memset(rbuf, 0xCD, (size_t)N * n);

        MPI_Alltoall(sbuf, n, MPI_BYTE, rbuf, n, MPI_BYTE, MPI_COMM_WORLD);

        for (p = 0; p < N; p++) {
            char *r = rbuf + (size_t)p * n;
            for (off = 0; off < n; off++) {
                uint8_t want = pat(k, n, p, rank, off);
                if ((uint8_t)r[off] != want) {
                    if (mism < 5) {
                        fprintf(stderr,
                                "A2A MISMATCH rank=%d size=%d iter=%d "
                                "from=%d off=%d got=0x%02x want=0x%02x\n",
                                rank, n, k, p, off,
                                (uint8_t)r[off], want);
                    }
                    mism++;
                }
            }
        }
    }
    return mism;
}

int main(int argc, char **argv) {
    int rank, N, i, max_size = 0;
    char *sbuf, *rbuf;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &N);

    if (N < 2) {
        if (rank == 0) {
            fprintf(stderr, "mpi_multi_v2 needs >= 2 ranks (got %d)\n", N);
        }
        MPI_Finalize();
        return 1;
    }

    for (i = 0; i < nsizes; i++) {
        if (sizes[i] > max_size) max_size = sizes[i];
    }
    /* a2a needs N*max_size per buffer; ring just needs max_size. */
    sbuf = (char *)malloc((size_t)N * max_size);
    rbuf = (char *)malloc((size_t)N * max_size);
    if (!sbuf || !rbuf) { MPI_Abort(MPI_COMM_WORLD, 1); }

    if (rank == 0) {
        printf("ranks=%d  iters=%d\n", N, ITERS);
        printf("%-8s %-10s %-10s %-10s\n",
               "phase", "bytes", "iters", "result");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n = sizes[i];
        long ring_local, a2a_local, ring_total = 0, a2a_total = 0;

        ring_local = ring_phase(rank, N, sbuf, rbuf, n);
        MPI_Reduce(&ring_local, &ring_total, 1, MPI_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-8s %-10d %-10d %s%s",
                   "ring", n, ITERS,
                   ring_total == 0 ? "OK" : "FAIL",
                   ring_total == 0 ? "\n" : "");
            if (ring_total != 0) printf(" (%ld bad bytes)\n", ring_total);
            fflush(stdout);
        }
        if (ring_total != 0) MPI_Abort(MPI_COMM_WORLD, 2);

        a2a_local = a2a_phase(rank, N, sbuf, rbuf, n);
        MPI_Reduce(&a2a_local, &a2a_total, 1, MPI_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-8s %-10d %-10d %s%s",
                   "a2a", n, ITERS,
                   a2a_total == 0 ? "OK" : "FAIL",
                   a2a_total == 0 ? "\n" : "");
            if (a2a_total != 0) printf(" (%ld bad bytes)\n", a2a_total);
            fflush(stdout);
        }
        if (a2a_total != 0) MPI_Abort(MPI_COMM_WORLD, 2);
    }

    free(sbuf);
    free(rbuf);
    MPI_Finalize();
    return 0;
}
