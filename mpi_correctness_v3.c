/*
 * mpi_correctness_v3.c -- 2-rank bidirectional byte correctness for obmm v3.
 *
 * This test keeps the V2 short/bcopy/fragment boundary coverage, then adds
 * the default V3 hybrid CC chunk boundary at 16 KiB. Both ranks send and
 * receive concurrently so hybrid mode exercises CC ownership in both
 * directions.
 *
 * Tunables:
 *   OBMM_TEST_ITERS     default 100
 *   OBMM_TEST_WINDOW    default 8
 *   OBMM_TEST_MAX_SIZE  default 1048576
 */
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int sizes[] = {
    1, 7, 8, 9, 64, 255, 256, 1024,
    2031, 2032, 2033,
    4095, 4096, 4097,
    8191, 8192, 8193,
    16383, 16384, 16385,
    32768, 65536, 262144, 1048576
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

static uint8_t pat(int iter, int size, int src, int dst, int win, int off)
{
    uint32_t h = (uint32_t)iter * 1315423911u
               ^ (uint32_t)size * 374761393u
               ^ (uint32_t)src  * 2246822519u
               ^ (uint32_t)dst  * 3266489917u
               ^ (uint32_t)win  * 668265263u
               ^ (uint32_t)off  * 2654435761u;
    return (uint8_t)(h ^ (h >> 16));
}

static void fill_pattern(char *buf, int n, int iter, int src, int dst,
                         int window)
{
    int w, off;

    for (w = 0; w < window; w++) {
        char *b = buf + (size_t)w * n;
        for (off = 0; off < n; off++) {
            b[off] = (char)pat(iter, n, src, dst, w, off);
        }
    }
}

static long verify_pattern(char *buf, int n, int iter, int src, int dst,
                           int window, int rank)
{
    long mismatches = 0;
    int w, off;

    for (w = 0; w < window; w++) {
        char *b = buf + (size_t)w * n;
        for (off = 0; off < n; off++) {
            uint8_t want = pat(iter, n, src, dst, w, off);
            if ((uint8_t)b[off] != want) {
                if (mismatches < 5) {
                    fprintf(stderr,
                            "MISMATCH rank=%d size=%d iter=%d win=%d "
                            "off=%d got=0x%02x want=0x%02x src=%d\n",
                            rank, n, iter, w, off, (uint8_t)b[off], want,
                            src);
                }
                mismatches++;
            }
        }
    }

    return mismatches;
}

int main(int argc, char **argv)
{
    int rank, nranks, peer, iters, window, max_size, nsizes, i, k;
    char *sbuf, *rbuf;
    MPI_Request *reqs;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (nranks != 2) {
        if (rank == 0) {
            fprintf(stderr, "mpi_correctness_v3 needs exactly 2 ranks (got %d)\n",
                    nranks);
        }
        MPI_Finalize();
        return 1;
    }

    iters = env_int("OBMM_TEST_ITERS", 100, 1, rank);
    window = env_int("OBMM_TEST_WINDOW", 8, 1, rank);
    max_size = env_int("OBMM_TEST_MAX_SIZE", 1048576, 1, rank);
    nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    peer = 1 - rank;

    sbuf = (char *)malloc((size_t)max_size * window);
    rbuf = (char *)malloc((size_t)max_size * window);
    reqs = (MPI_Request *)malloc((size_t)2 * window * sizeof(*reqs));
    if (sbuf == NULL || rbuf == NULL || reqs == NULL) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    if (rank == 0) {
        printf("ranks=2 iters=%d window=%d max_size=%d\n",
               iters, window, max_size);
        printf("%-10s %-10s %-8s %-10s\n",
               "bytes", "iters", "window", "result");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n = sizes[i];
        long local_mismatches = 0;
        long total_mismatches = 0;

        if (n > max_size) {
            continue;
        }

        for (k = 0; k < iters; k++) {
            int w;

            fill_pattern(sbuf, n, k, rank, peer, window);
            memset(rbuf, 0xCD, (size_t)n * window);

            for (w = 0; w < window; w++) {
                MPI_Irecv(rbuf + (size_t)w * n, n, MPI_BYTE, peer, 300,
                          MPI_COMM_WORLD, &reqs[w]);
            }
            for (w = 0; w < window; w++) {
                MPI_Isend(sbuf + (size_t)w * n, n, MPI_BYTE, peer, 300,
                          MPI_COMM_WORLD, &reqs[window + w]);
            }

            MPI_Waitall(2 * window, reqs, MPI_STATUSES_IGNORE);
            local_mismatches += verify_pattern(rbuf, n, k, peer, rank,
                                               window, rank);
        }

        MPI_Reduce(&local_mismatches, &total_mismatches, 1, MPI_LONG, MPI_SUM,
                   0, MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-10d %-10d %-8d %s%s", n, iters, window,
                   total_mismatches == 0 ? "OK" : "FAIL",
                   total_mismatches == 0 ? "\n" : "");
            if (total_mismatches != 0) {
                printf(" (%ld bad bytes)\n", total_mismatches);
            }
            fflush(stdout);
        }
        if (total_mismatches != 0) {
            MPI_Abort(MPI_COMM_WORLD, 2);
        }
    }

    free(reqs);
    free(rbuf);
    free(sbuf);
    MPI_Finalize();
    return 0;
}
