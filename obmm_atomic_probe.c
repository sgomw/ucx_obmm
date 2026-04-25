/*
 * obmm_atomic_probe.c
 *
 * Probe whether atomic ops on a remote OBMM NC mapping are safe.
 *
 * Build (on build host):
 *   aarch64-linux-gnu-gcc -O2 -Wall -o obmm_atomic_probe obmm_atomic_probe.c
 * or natively on ARM64:
 *   gcc -O2 -Wall -o obmm_atomic_probe obmm_atomic_probe.c
 *
 * Copy the resulting binary to the run host.
 *
 * Run:
 *   ./obmm_atomic_probe /dev/obmm_shmdevN [bytes]
 *
 * Steps run in order. Each step prints "STEP n: <name>" then flushes,
 * sleeps 1s, executes, then prints "STEP n: OK". If the system hangs,
 * the last "STEP n: <name>" you see on screen is the offending op.
 *
 * Use --skip-cas to stop before any LDXR/STXR/CAS is executed (safe mode).
 * Use --loops N to override the final CAS loop count (default 1000).
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_LEN  (4096)

static void step_begin(int n, const char *name) {
    printf("STEP %d: %s ...\n", n, name);
    fflush(stdout);
    sleep(1);
}

static void step_ok(int n) {
    printf("STEP %d: OK\n", n);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    size_t len = DEFAULT_LEN;
    int skip_cas = 0;
    long loops = 1000;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--skip-cas") == 0) {
            skip_cas = 1;
        } else if (strcmp(argv[i], "--loops") == 0 && i + 1 < argc) {
            loops = atol(argv[++i]);
        } else if (path == NULL) {
            path = argv[i];
        } else {
            len = (size_t)strtoull(argv[i], NULL, 0);
        }
    }

    if (path == NULL) {
        fprintf(stderr,
                "usage: %s /dev/obmm_shmdevN [bytes] [--skip-cas] [--loops N]\n",
                argv[0]);
        return 2;
    }

    printf("path=%s len=%zu skip_cas=%d loops=%ld\n",
           path, len, skip_cas, loops);
    fflush(stdout);

    step_begin(0, "open(O_RDWR|O_SYNC)");
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }
    step_ok(0);

    step_begin(1, "mmap(PROT_RW, MAP_SHARED, offset=0)");
    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("  map=%p\n", map);
    fflush(stdout);
    step_ok(1);

    volatile uint64_t *p = (volatile uint64_t *)map;

    step_begin(2, "plain 8B store *p = 0xA1A1...");
    *p = 0xA1A1A1A1A1A1A1A1ULL;
    step_ok(2);

    step_begin(3, "plain 8B load v = *p");
    uint64_t v = *p;
    printf("  read back 0x%016" PRIx64 "\n", v);
    fflush(stdout);
    step_ok(3);

    step_begin(4, "__atomic_store_n RELEASE *p = 0xB2B2...");
    __atomic_store_n((uint64_t *)p, 0xB2B2B2B2B2B2B2B2ULL, __ATOMIC_RELEASE);
    step_ok(4);

    step_begin(5, "__atomic_load_n ACQUIRE v = *p");
    v = __atomic_load_n((uint64_t *)p, __ATOMIC_ACQUIRE);
    printf("  read back 0x%016" PRIx64 "\n", v);
    fflush(stdout);
    step_ok(5);

    if (skip_cas) {
        printf("--skip-cas set, stopping before any RMW.\n");
        munmap(map, len);
        close(fd);
        return 0;
    }

    /* From here on we issue exclusive / atomic RMW ops. On ARM64 these are
     * LDXR/STXR (no LSE) or CAS/LDADD (LSE). If the device memory backing
     * this mapping does not tolerate them, the SoC may SError or hang the
     * IO bus from this point onward. */

    step_begin(6, "__atomic_fetch_add 8B  (one op)");
    uint64_t prev = __atomic_fetch_add((uint64_t *)p, 1, __ATOMIC_ACQ_REL);
    printf("  prev=0x%016" PRIx64 "\n", prev);
    fflush(stdout);
    step_ok(6);

    step_begin(7, "__atomic_compare_exchange_n 8B  (one op, expected match)");
    uint64_t expected = prev + 1;
    uint64_t desired  = 0xC3C3C3C3C3C3C3C3ULL;
    int swapped = __atomic_compare_exchange_n((uint64_t *)p, &expected,
                                              desired, 0,
                                              __ATOMIC_ACQ_REL,
                                              __ATOMIC_ACQUIRE);
    printf("  swapped=%d expected_after=0x%016" PRIx64 "\n",
           swapped, expected);
    fflush(stdout);
    step_ok(7);

    step_begin(8, "CAS loop x N (uncontended, expected hit every time)");
    long ok = 0;
    for (long k = 0; k < loops; k++) {
        uint64_t e = __atomic_load_n((uint64_t *)p, __ATOMIC_ACQUIRE);
        uint64_t d = e + 1;
        if (__atomic_compare_exchange_n((uint64_t *)p, &e, d, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            ok++;
        }
        if ((k & 0xff) == 0) {
            printf("  k=%ld ok=%ld cur=0x%016" PRIx64 "\n", k, ok, d);
            fflush(stdout);
        }
    }
    printf("  total_ok=%ld / %ld final=0x%016" PRIx64 "\n",
           ok, loops, *p);
    fflush(stdout);
    step_ok(8);

    munmap(map, len);
    close(fd);
    printf("ALL STEPS COMPLETED\n");
    return 0;
}