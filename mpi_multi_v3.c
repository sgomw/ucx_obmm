/*
 * mpi_multi_v3.c -- N-rank correctness/load test for obmm v3.
 *
 * Phases:
 *   1) ring: every rank sends to (rank + 1) % N and receives from
 *      (rank - 1 + N) % N.
 *   2) alltoall: every rank exchanges with every other rank.
 *
 * Sizes cover short, NC bcopy, the default 16 KiB hybrid CC chunk boundary,
 * and UCP-fragmented messages. Use MULTI_NP in run_mpi_tests_v3.sh to scale
 * rank count after 2-rank tests pass.
 *
 * Tunables:
 *   OBMM_TEST_ITERS     default 50
 *   OBMM_TEST_MAX_SIZE  default 65536
 */
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int sizes[] = {
    1, 64, 1024,
    2032, 2033,
    4096, 4097,
    8192,
    16384, 16385,
    32768, 65536
};

static int env_int(const char *name, int defval, int minval, int rank)
{
    const char *s = getenv(name);
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0') {
        return defval;
    }

    v = strtol(s, &end, 10);
    if (*end != '\0' || v < minval || v > 2147483647L) {
        if (rank == 0) {
            fprintf(stderr, "Invalid %s='%s' (expected integer >= %d)\n",
                    name, s, minval);
        }
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    return (int)v;
}

static uint8_t pat(int iter, int size, int src, int dst, int off)
{
    uint32_t h = (uint32_t)iter * 1315423911u
               ^ (uint32_t)size * 374761393u
               ^ (uint32_t)src  * 2246822519u
               ^ (uint32_t)dst  * 3266489917u
               ^ (uint32_t)off  * 2654435761u;
    return (uint8_t)(h ^ (h >> 16));
}

static long ring_phase(int rank, int nranks, int iters, char *sbuf,
                       char *rbuf, int n)
{
    int dst = (rank + 1) % nranks;
    int src = (rank - 1 + nranks) % nranks;
    long mismatches = 0;
    int k, off;
    MPI_Request reqs[2];

    for (k = 0; k < iters; k++) {
        for (off = 0; off < n; off++) {
            sbuf[off] = (char)pat(k, n, rank, dst, off);
        }
        memset(rbuf, 0xCD, n);

        MPI_Irecv(rbuf, n, MPI_BYTE, src, 700, MPI_COMM_WORLD, &reqs[0]);
        MPI_Isend(sbuf, n, MPI_BYTE, dst, 700, MPI_COMM_WORLD, &reqs[1]);
        MPI_Waitall(2, reqs, MPI_STATUSES_IGNORE);

        for (off = 0; off < n; off++) {
            uint8_t want = pat(k, n, src, rank, off);
            if ((uint8_t)rbuf[off] != want) {
                if (mismatches < 5) {
                    fprintf(stderr,
                            "RING MISMATCH rank=%d size=%d iter=%d off=%d "
                            "got=0x%02x want=0x%02x src=%d\n",
                            rank, n, k, off, (uint8_t)rbuf[off], want, src);
                }
                mismatches++;
            }
        }
    }

    return mismatches;
}

static long alltoall_phase(int rank, int nranks, int iters, char *sbuf,
                           char *rbuf, int n)
{
    long mismatches = 0;
    int k, p, off;

    for (k = 0; k < iters; k++) {
        for (p = 0; p < nranks; p++) {
            char *s = sbuf + (size_t)p * n;
            for (off = 0; off < n; off++) {
                s[off] = (char)pat(k, n, rank, p, off);
            }
        }
        memset(rbuf, 0xCD, (size_t)nranks * n);

        MPI_Alltoall(sbuf, n, MPI_BYTE, rbuf, n, MPI_BYTE, MPI_COMM_WORLD);

        for (p = 0; p < nranks; p++) {
            char *r = rbuf + (size_t)p * n;
            for (off = 0; off < n; off++) {
                uint8_t want = pat(k, n, p, rank, off);
                if ((uint8_t)r[off] != want) {
                    if (mismatches < 5) {
                        fprintf(stderr,
                                "A2A MISMATCH rank=%d size=%d iter=%d "
                                "from=%d off=%d got=0x%02x want=0x%02x\n",
                                rank, n, k, p, off, (uint8_t)r[off], want);
                    }
                    mismatches++;
                }
            }
        }
    }

    return mismatches;
}

int main(int argc, char **argv)
{
    int rank, nranks, iters, max_size, nsizes, i;
    char *sbuf, *rbuf;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (nranks < 2) {
        if (rank == 0) {
            fprintf(stderr, "mpi_multi_v3 needs >= 2 ranks (got %d)\n",
                    nranks);
        }
        MPI_Finalize();
        return 1;
    }

    iters = env_int("OBMM_TEST_ITERS", 50, 1, rank);
    max_size = env_int("OBMM_TEST_MAX_SIZE", 65536, 1, rank);
    nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    sbuf = (char *)malloc((size_t)nranks * max_size);
    rbuf = (char *)malloc((size_t)nranks * max_size);
    if (sbuf == NULL || rbuf == NULL) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    if (rank == 0) {
        printf("ranks=%d iters=%d max_size=%d\n", nranks, iters, max_size);
        printf("%-8s %-10s %-10s %-10s\n",
               "phase", "bytes", "iters", "result");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n = sizes[i];
        long ring_local, a2a_local, ring_total = 0, a2a_total = 0;

        if (n > max_size) {
            continue;
        }

        ring_local = ring_phase(rank, nranks, iters, sbuf, rbuf, n);
        MPI_Reduce(&ring_local, &ring_total, 1, MPI_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-8s %-10d %-10d %s%s", "ring", n, iters,
                   ring_total == 0 ? "OK" : "FAIL",
                   ring_total == 0 ? "\n" : "");
            if (ring_total != 0) {
                printf(" (%ld bad bytes)\n", ring_total);
            }
            fflush(stdout);
        }
        if (ring_total != 0) {
            MPI_Abort(MPI_COMM_WORLD, 2);
        }

        a2a_local = alltoall_phase(rank, nranks, iters, sbuf, rbuf, n);
        MPI_Reduce(&a2a_local, &a2a_total, 1, MPI_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-8s %-10d %-10d %s%s", "a2a", n, iters,
                   a2a_total == 0 ? "OK" : "FAIL",
                   a2a_total == 0 ? "\n" : "");
            if (a2a_total != 0) {
                printf(" (%ld bad bytes)\n", a2a_total);
            }
            fflush(stdout);
        }
        if (a2a_total != 0) {
            MPI_Abort(MPI_COMM_WORLD, 2);
        }
    }

    free(rbuf);
    free(sbuf);
    MPI_Finalize();
    return 0;
}
