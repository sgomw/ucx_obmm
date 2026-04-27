/* obmm_pool_reset.c
 *
 * Zero the head of the obmm-exported region so the next UCX run
 * re-initializes the pool from scratch. Use this after bumping
 * UCT_OBMM_POOL_VERSION (or any pool layout change).
 *
 * MUST be run on every node, while NO MPI/UCX job is touching the region.
 *
 * Build:  gcc -O2 -o obmm_pool_reset obmm_pool_reset.c
 * Run:    ./obmm_pool_reset /dev/obmm/<dev>   (path to the same chardev
 *         UCX mmaps; check ucx_info -d output / sysfs)
 *
 * Zeros the first 1 MiB. That covers pool_hdr (~64B) + bitmap + slot_meta
 * for any reasonable slot_count, with margin. Slot bodies further in are
 * left untouched (safe — they're re-init'd by senders/receivers as they
 * publish, and the receiver's owner-bit walk starts from a zeroed lap).
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define ZERO_BYTES (1UL << 20)   /* 1 MiB */

int main(int argc, char **argv)
{
    int   fd;
    void *p;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <obmm-chardev-path>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    p = mmap(NULL, ZERO_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    memset(p, 0, ZERO_BYTES);
    /* fence to push the writes out of the CPU before we let the FD close */
    __sync_synchronize();

    munmap(p, ZERO_BYTES);
    close(fd);

    printf("obmm: zeroed first %lu bytes of %s\n",
           (unsigned long)ZERO_BYTES, argv[1]);
    return 0;
}
