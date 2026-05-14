/*
 * mpi_pressure_v3.c -- sustained bidirectional pressure for obmm v3.
 *
 * Default size is 32 KiB, which is above the default 16 KiB hybrid bcopy cap
 * and should force UCP to fragment each MPI message into multiple AM bcopy
 * operations. The default window intentionally exceeds the conservative
 * per-slot CC chunk count to exercise pending and reclaim.
 *
 * Tunables:
 *   OBMM_PRESSURE_ITERS   default 200
 *   OBMM_PRESSURE_WINDOW  default 64
 *   OBMM_PRESSURE_SIZE    default 32768
 */
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint8_t pat(int iter, int src, int dst, int seq, int off)
{
    uint32_t h = (uint32_t)iter * 1315423911u
               ^ (uint32_t)src  * 2246822519u
               ^ (uint32_t)dst  * 3266489917u
               ^ (uint32_t)seq  * 668265263u
               ^ (uint32_t)off  * 2654435761u;
    return (uint8_t)(h ^ (h >> 16));
}

int main(int argc, char **argv)
{
    int rank, nranks, peer, iters, window, msg_size;
    char *sbuf, *rbuf;
    MPI_Request *reqs;
    long local_mismatches = 0, total_mismatches = 0;
    int k;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    if (nranks != 2) {
        if (rank == 0) {
            fprintf(stderr, "mpi_pressure_v3 needs exactly 2 ranks (got %d)\n",
                    nranks);
        }
        MPI_Finalize();
        return 1;
    }

    iters = env_int("OBMM_PRESSURE_ITERS", 200, 1, rank);
    window = env_int("OBMM_PRESSURE_WINDOW", 64, 1, rank);
    msg_size = env_int("OBMM_PRESSURE_SIZE", 32768, 1, rank);
    peer = 1 - rank;

    sbuf = (char *)malloc((size_t)msg_size * window);
    rbuf = (char *)malloc((size_t)msg_size * window);
    reqs = (MPI_Request *)malloc((size_t)2 * window * sizeof(*reqs));
    if (sbuf == NULL || rbuf == NULL || reqs == NULL) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    if (rank == 0) {
        printf("ranks=2 iters=%d window=%d msg_size=%d\n",
               iters, window, msg_size);
        fflush(stdout);
    }

    for (k = 0; k < iters; k++) {
        int w, off;

        for (w = 0; w < window; w++) {
            char *s = sbuf + (size_t)w * msg_size;
            for (off = 0; off < msg_size; off++) {
                s[off] = (char)pat(k, rank, peer, w, off);
            }
        }
        memset(rbuf, 0xCD, (size_t)msg_size * window);

        for (w = 0; w < window; w++) {
            MPI_Irecv(rbuf + (size_t)w * msg_size, msg_size, MPI_BYTE, peer,
                      500, MPI_COMM_WORLD, &reqs[w]);
        }
        for (w = 0; w < window; w++) {
            MPI_Isend(sbuf + (size_t)w * msg_size, msg_size, MPI_BYTE, peer,
                      500, MPI_COMM_WORLD, &reqs[window + w]);
        }

        MPI_Waitall(2 * window, reqs, MPI_STATUSES_IGNORE);

        for (w = 0; w < window; w++) {
            char *r = rbuf + (size_t)w * msg_size;
            for (off = 0; off < msg_size; off++) {
                uint8_t want = pat(k, peer, rank, w, off);
                if ((uint8_t)r[off] != want) {
                    if (local_mismatches < 5) {
                        fprintf(stderr,
                                "PRESSURE MISMATCH rank=%d iter=%d win=%d "
                                "off=%d got=0x%02x want=0x%02x\n",
                                rank, k, w, off, (uint8_t)r[off], want);
                    }
                    local_mismatches++;
                }
            }
        }

        if (rank == 0 && ((k + 1) % 25 == 0 || k + 1 == iters)) {
            printf("progress %d/%d\n", k + 1, iters);
            fflush(stdout);
        }
    }

    MPI_Reduce(&local_mismatches, &total_mismatches, 1, MPI_LONG, MPI_SUM, 0,
               MPI_COMM_WORLD);
    if (rank == 0) {
        printf("pressure result: %s%s", total_mismatches == 0 ? "OK" : "FAIL",
               total_mismatches == 0 ? "\n" : "");
        if (total_mismatches != 0) {
            printf(" (%ld bad bytes)\n", total_mismatches);
        }
        fflush(stdout);
    }
    if (total_mismatches != 0) {
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    free(reqs);
    free(rbuf);
    free(sbuf);
    MPI_Finalize();
    return 0;
}
