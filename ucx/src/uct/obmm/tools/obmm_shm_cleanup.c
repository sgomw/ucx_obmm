/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

/**
 * obmm shared-memory region cleanup utility.
 *
 * Opens an obmm shmdev with O_SYNC (non-cacheable), mmaps the entire region,
 * zeroes it with a bus-domain fence so the reset is visible to any future
 * O_SYNC opener, then unmaps and closes.
 *
 * Usage:
 *   obmm_shm_cleanup <memid> <size>
 *
 *   memid   — decimal memid of the shmdev (e.g. 1 for /dev/obmm_shmdev1)
 *   size    — region size in bytes; accepts decimal (268435456) or hex
 *             (0x10000000)
 *
 * Example:
 *   obmm_shm_cleanup 1 0x10000000    # zero /dev/obmm_shmdev1, 256 MiB
 *
 * Intended for manual cleanup after all UCX processes on this node have
 * exited and before the next run. Not safe to run while any iface still
 * holds the region mapped.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#define OBMM_DEV_PATH_FMT  "/dev/obmm_shmdev%" PRIu64
#define OBMM_PATH_MAX      256

static void bus_full_fence(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb osh" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("mfence" ::: "memory");
#elif defined(__powerpc64__)
    __asm__ __volatile__("sync" ::: "memory");
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
#else
    /* Fallback: store + load fence covers most archs. */
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void die(const char *msg)
{
    fprintf(stderr, "obmm_shm_cleanup: %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static uint64_t parse_u64(const char *s, const char *name)
{
    char    *end;
    uint64_t v;

    v = strtoull(s, &end, 0);
    if (*end != '\0' || end == s) {
        fprintf(stderr, "obmm_shm_cleanup: invalid %s '%s'\n", name, s);
        exit(EXIT_FAILURE);
    }
    return v;
}

int main(int argc, char *argv[])
{
    uint64_t memid;
    uint64_t size;
    char     dev_path[OBMM_PATH_MAX];
    int      fd;
    void    *map;

    if (argc != 3) {
        fprintf(stderr, "Usage: obmm_shm_cleanup <memid> <size>\n");
        fprintf(stderr, "  memid  — decimal memid (e.g. 1 → /dev/obmm_shmdev1)\n");
        fprintf(stderr, "  size   — region size, decimal or hex (e.g. 0x10000000)\n");
        return EXIT_FAILURE;
    }

    memid = parse_u64(argv[1], "memid");
    size  = parse_u64(argv[2], "size");

    if (memid == 0) {
        fprintf(stderr, "obmm_shm_cleanup: memid must be non-zero\n");
        return EXIT_FAILURE;
    }
    if (size == 0) {
        fprintf(stderr, "obmm_shm_cleanup: size must be non-zero\n");
        return EXIT_FAILURE;
    }

    snprintf(dev_path, sizeof(dev_path), OBMM_DEV_PATH_FMT, memid);
    printf("obmm_shm_cleanup: opening %s (size=0x%" PRIx64 " %" PRIu64 " MiB)\n",
           dev_path, size, size / (1024 * 1024));

    fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        die("open");
    }

    map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("obmm_shm_cleanup: mmap");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("obmm_shm_cleanup: mapped at %p\n", map);

    /* Zero the entire region. This resets the pool header (magic→0,
     * state→UNINIT), the slot bitmap, all slot_meta, and every slot's
     * FIFO ctl / elements / descs / short lanes — matching the semantics
     * of uct_obmm_pool_reset(). */
    printf("obmm_shm_cleanup: zeroing %" PRIu64 " MiB ...\n",
           size / (1024 * 1024));
    fflush(stdout);

    memset(map, 0, (size_t)size);

    /* Bus-domain full fence: ensures the zeroed bytes are visible through
     * NC mappings before we unmap. Matches the ordering in pool_reset(). */
    bus_full_fence();

    printf("obmm_shm_cleanup: done.\n");

    if (munmap(map, (size_t)size) != 0) {
        perror("obmm_shm_cleanup: munmap");
    }
    close(fd);

    return EXIT_SUCCESS;
}
