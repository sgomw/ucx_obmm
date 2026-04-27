/*
 * mpi_correctness_v2.c -- 2-rank byte-level correctness across the FULL
 * obmm protocol range: am_short (<=2032), am_bcopy (<=BCOPY_SEG_SIZE),
 * and UCP-fragmented messages (above BCOPY_SEG_SIZE).
 *
 * Differences vs v1 (mpi_correctness.c):
 *   - sizes go up to 1 MiB (forces UCP fragmentation over am_bcopy)
 *   - hits exact boundary values 2031/2032/2033 and 4095/4096/4097
 *   - drives a window of in-flight Isend/Irecv to stress FIFO slot reuse
 *     and desc-paired lifetime under concurrency
 *   - per-(iter,size) keyed pattern so a stale slot from a prior size
 *     produces a distinguishable mismatch report
 *
 * Failure mode: prints first 5 mismatches with offset/expected/actual,
 * then MPI_Abort.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ITERS   200
#define WINDOW  16

/* Boundaries: just-under / at / just-over am_short (2032),
 * default am_bcopy (4096), and a wide sweep into UCP fragmentation. */
static const int sizes[] = {
    1, 7, 8, 9, 64, 255, 256, 1024,
    /* short boundary */
    2031, 2032, 2033,
    /* mid-range bcopy */
    3000,
    /* bcopy boundary (default seg_size = 4096) */
    4095, 4096, 4097,
    /* fragmented over multiple bcopy sends */
    8192, 16384, 65536, 262144, 1048576
};
static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);

static uint8_t pat(int iter, int sz, int off) {
    uint32_t h = (uint32_t)iter * 1315423911u
               ^ (uint32_t)sz   * 374761393u
               ^ (uint32_t)off  * 2654435761u;
    return (uint8_t)(h ^ (h >> 16));
}

int main(int argc, char **argv) {
    int rank, size, i, k, w, off;
    int max_size = 0;
    char *bufs;
    MPI_Request reqs[WINDOW];

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != 2) {
        if (rank == 0) {
            fprintf(stderr,
                    "mpi_correctness_v2 needs exactly 2 ranks (got %d)\n",
                    size);
        }
        MPI_Finalize();
        return 1;
    }

    for (i = 0; i < nsizes; i++) {
        if (sizes[i] > max_size) max_size = sizes[i];
    }
    /* WINDOW separate buffers so concurrent Isends/Irecvs don't alias */
    bufs = (char *)malloc((size_t)max_size * WINDOW);
    if (!bufs) { MPI_Abort(MPI_COMM_WORLD, 1); }

    if (rank == 0) {
        printf("%-10s %-10s %-8s %-10s\n", "bytes", "iters", "window",
               "result");
        fflush(stdout);
    }

    for (i = 0; i < nsizes; i++) {
        int n = sizes[i];
        long mismatches = 0;

        for (k = 0; k < ITERS; k++) {
            if (rank == 0) {
                for (w = 0; w < WINDOW; w++) {
                    char *b = bufs + (size_t)w * n;
                    for (off = 0; off < n; off++) {
                        b[off] = (char)pat(k * WINDOW + w, n, off);
                    }
                    MPI_Isend(b, n, MPI_BYTE, 1, 0, MPI_COMM_WORLD,
                              &reqs[w]);
                }
                MPI_Waitall(WINDOW, reqs, MPI_STATUSES_IGNORE);
            } else {
                /* poison receive buffers so any short-write is visible */
                memset(bufs, 0xCD, (size_t)n * WINDOW);
                for (w = 0; w < WINDOW; w++) {
                    MPI_Irecv(bufs + (size_t)w * n, n, MPI_BYTE, 0, 0,
                              MPI_COMM_WORLD, &reqs[w]);
                }
                MPI_Waitall(WINDOW, reqs, MPI_STATUSES_IGNORE);
                for (w = 0; w < WINDOW; w++) {
                    char *b = bufs + (size_t)w * n;
                    for (off = 0; off < n; off++) {
                        uint8_t want = pat(k * WINDOW + w, n, off);
                        if ((uint8_t)b[off] != want) {
                            if (mismatches < 5) {
                                fprintf(stderr,
                                        "MISMATCH size=%d iter=%d win=%d "
                                        "off=%d got=0x%02x want=0x%02x\n",
                                        n, k, w, off,
                                        (uint8_t)b[off], want);
                            }
                            mismatches++;
                        }
                    }
                }
            }
        }

        long total = 0;
        MPI_Reduce(&mismatches, &total, 1, MPI_LONG, MPI_SUM, 0,
                   MPI_COMM_WORLD);
        if (rank == 0) {
            printf("%-10d %-10d %-8d %s%s",
                   n, ITERS, WINDOW,
                   total == 0 ? "OK" : "FAIL",
                   total == 0 ? "\n" : "");
            if (total != 0) printf(" (%ld bad bytes)\n", total);
            fflush(stdout);
        }
        if (total != 0) {
            MPI_Abort(MPI_COMM_WORLD, 2);
        }
    }

    free(bufs);
    MPI_Finalize();
    return 0;
}
