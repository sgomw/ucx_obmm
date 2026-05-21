/*
 * obmm_atomic_probe.c
 *
 * Minimal NC-mapping atomic probe for obmm shmdev pages.
 *
 * Purpose:
 *   1. Reproduce the current 64-bit shared lock protocol outside UCX
 *   2. Check whether 64-bit FAA return values are self-consistent on NC
 *
 * Build:
 *   gcc -O2 -std=gnu11 -o obmm_atomic_probe obmm_atomic_probe.c
 *
 * Note:
 *   On arm64, this probe uses explicit LSE atomic instructions (`casal`,
 *   `ldaddal`) instead of compiler-default atomics, because the target NC
 *   mapping may support LSE atomics but not LL/SC sequences.
 *
 * Typical use:
 *   1. Pick an offset that is not used by UCX. By default this tool uses the
 *      last page of the 128 MiB obmm region.
 *   2. Make sure no UCX/MPI job is touching the same probe page.
 *   3. Clear once:
 *        ./obmm_atomic_probe --dev /dev/obmm_shmdevX --mode clear
 *   4. Run the same mode concurrently on the two nodes against two device
 *      paths that map the SAME underlying obmm region:
 *        ./obmm_atomic_probe --dev /dev/obmm_shmdevX --mode lock64 --iters 2000000
 *        ./obmm_atomic_probe --dev /dev/obmm_shmdevY --mode lock64 --iters 2000000
 *      and then
 *        ./obmm_atomic_probe --dev /dev/obmm_shmdevX --mode faa64 --iters 2000000
 *        ./obmm_atomic_probe --dev /dev/obmm_shmdevY --mode faa64 --iters 2000000
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define OBMM_PROBE_REGION_BYTES (128UL << 20)
#define OBMM_PROBE_DEFAULT_OFFSET (OBMM_PROBE_REGION_BYTES - 4096UL)
#define OBMM_PROBE_DEFAULT_ITERS 2000000UL
#define OBMM_PROBE_DEFAULT_ANOMALY_LIMIT 8UL

typedef enum probe_mode {
    PROBE_MODE_CLEAR = 0,
    PROBE_MODE_LOCK64,
    PROBE_MODE_FAA64
} probe_mode_t;

typedef struct probe_args {
    const char   *dev_path;
    probe_mode_t  mode;
    uint64_t      offset;
    uint64_t      iters;
    uint64_t      token;
    uint64_t      hold_loops;
    uint64_t      anomaly_limit;
} probe_args_t;

typedef struct probe_page {
    volatile uint64_t lock_word;
    volatile uint64_t head_word;
    volatile uint64_t faa_word;
    volatile uint64_t reserved;
} probe_page_t;

typedef struct mapped_probe {
    int           fd;
    void         *map_base;
    size_t        map_length;
    probe_page_t *page;
} mapped_probe_t;

static inline void probe_bus_store_fence(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb oshst" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("sfence" ::: "memory");
#elif defined(__powerpc64__)
    __asm__ __volatile__("lwsync" ::: "memory");
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ __volatile__("fence ow,ow" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void probe_bus_load_fence(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb oshld" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("lfence" ::: "memory");
#elif defined(__powerpc64__)
    __asm__ __volatile__("lwsync" ::: "memory");
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ __volatile__("fence ir,ir" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void probe_bus_full_fence(void)
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
    __sync_synchronize();
#endif
}

static inline uint64_t probe_atomic_cswap64(volatile uint64_t *ptr,
                                            uint64_t oldval, uint64_t newval)
{
#if defined(__aarch64__)
    uint64_t observed = oldval;

    __asm__ __volatile__(
            ".arch_extension lse\n\t"
            "casal %x[observed], %x[newval], [%[ptr]]"
            : [observed] "+&r"(observed)
            : [newval] "r"(newval), [ptr] "r"(ptr)
            : "memory");
    return observed;
#else
    return __sync_val_compare_and_swap(ptr, oldval, newval);
#endif
}

static inline uint64_t probe_atomic_fetch_add64(volatile uint64_t *ptr,
                                                uint64_t add)
{
#if defined(__aarch64__)
    uint64_t oldval;

    __asm__ __volatile__(
            ".arch_extension lse\n\t"
            "ldaddal %x[add], %x[oldval], [%[ptr]]"
            : [oldval] "=&r"(oldval)
            : [add] "r"(add), [ptr] "r"(ptr)
            : "memory");
    return oldval;
#else
    return __sync_fetch_and_add(ptr, add);
#endif
}

static const char *probe_atomic_backend_name(void)
{
#if defined(__aarch64__)
    return "aarch64-explicit-lse(casal,ldaddal)";
#else
    return "compiler-builtin-fallback";
#endif
}

static uint64_t probe_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

static uint64_t probe_default_token(void)
{
    char hostname[256];
    uint64_t seed = (uint64_t)getpid();
    size_t i;

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strncpy(hostname, "unknown-host", sizeof(hostname));
        hostname[sizeof(hostname) - 1] = '\0';
    }

    for (i = 0; hostname[i] != '\0'; ++i) {
        seed = probe_mix64(seed ^ (unsigned char)hostname[i]);
    }

    seed = probe_mix64(seed ^ (uint64_t)time(NULL));
    return (seed == 0) ? UINT64_C(1) : seed;
}

static void probe_usage(const char *progname)
{
    fprintf(stderr,
            "usage: %s --dev <path> --mode <clear|lock64|faa64> [options]\n"
            "options:\n"
            "  --offset <bytes>       Probe offset inside shmdev "
            "(default: %lu)\n"
            "  --iters <count>        Iterations for lock64/faa64 "
            "(default: %lu)\n"
            "  --token <hex|dec>      Token for lock64 "
            "(default: auto-generated)\n"
            "  --hold-loops <count>   Busy-loop inside lock critical section\n"
            "  --anomaly-limit <n>    Max detailed anomaly lines per run "
            "(default: %lu)\n",
            progname, (unsigned long)OBMM_PROBE_DEFAULT_OFFSET,
            (unsigned long)OBMM_PROBE_DEFAULT_ITERS,
            (unsigned long)OBMM_PROBE_DEFAULT_ANOMALY_LIMIT);
}

static int probe_parse_u64(const char *str, uint64_t *value_p)
{
    char *endptr = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(str, &endptr, 0);
    if ((errno != 0) || (endptr == str) || ((endptr != NULL) && (*endptr != '\0'))) {
        return -1;
    }

    *value_p = (uint64_t)value;
    return 0;
}

static int probe_parse_args(int argc, char **argv, probe_args_t *args)
{
    int i;
    int mode_set = 0;

    memset(args, 0, sizeof(*args));
    args->offset        = OBMM_PROBE_DEFAULT_OFFSET;
    args->iters         = OBMM_PROBE_DEFAULT_ITERS;
    args->token         = probe_default_token();
    args->anomaly_limit = OBMM_PROBE_DEFAULT_ANOMALY_LIMIT;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dev")) {
            if (++i >= argc) {
                return -1;
            }
            args->dev_path = argv[i];
        } else if (!strcmp(argv[i], "--mode")) {
            if (++i >= argc) {
                return -1;
            }
            if (!strcmp(argv[i], "clear")) {
                args->mode = PROBE_MODE_CLEAR;
            } else if (!strcmp(argv[i], "lock64")) {
                args->mode = PROBE_MODE_LOCK64;
            } else if (!strcmp(argv[i], "faa64")) {
                args->mode = PROBE_MODE_FAA64;
            } else {
                return -1;
            }
            mode_set = 1;
        } else if (!strcmp(argv[i], "--offset")) {
            if ((++i >= argc) || (probe_parse_u64(argv[i], &args->offset) != 0)) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--iters")) {
            if ((++i >= argc) || (probe_parse_u64(argv[i], &args->iters) != 0)) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--token")) {
            if ((++i >= argc) || (probe_parse_u64(argv[i], &args->token) != 0)) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--hold-loops")) {
            if ((++i >= argc) || (probe_parse_u64(argv[i], &args->hold_loops) != 0)) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--anomaly-limit")) {
            if ((++i >= argc) || (probe_parse_u64(argv[i], &args->anomaly_limit) != 0)) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (args->dev_path == NULL) {
        return -1;
    }
    if (!mode_set) {
        return -1;
    }
    if (args->offset > (OBMM_PROBE_REGION_BYTES - sizeof(probe_page_t))) {
        fprintf(stderr, "offset %" PRIu64 " exceeds probeable region\n",
                args->offset);
        return -1;
    }

    return 0;
}

static int probe_map(const probe_args_t *args, mapped_probe_t *mapped)
{
    long page_size;
    off_t map_offset;
    size_t delta;
    struct stat st;

    memset(mapped, 0, sizeof(*mapped));
    mapped->fd = -1;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "failed to get page size\n");
        return -1;
    }

    map_offset = (off_t)(args->offset & ~((uint64_t)page_size - 1));
    delta      = (size_t)(args->offset - (uint64_t)map_offset);

    mapped->fd = open(args->dev_path, O_RDWR | O_SYNC);
    if (mapped->fd < 0) {
        perror("open");
        return -1;
    }
    if ((fstat(mapped->fd, &st) != 0) || !S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is not a character device\n", args->dev_path);
        close(mapped->fd);
        mapped->fd = -1;
        return -1;
    }

    mapped->map_length = (size_t)page_size;
    mapped->map_base   = mmap(NULL, mapped->map_length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, mapped->fd, map_offset);
    if (mapped->map_base == MAP_FAILED) {
        perror("mmap");
        close(mapped->fd);
        mapped->fd = -1;
        return -1;
    }

    if ((delta + sizeof(probe_page_t)) > mapped->map_length) {
        fprintf(stderr, "probe structure does not fit in mapped page\n");
        munmap(mapped->map_base, mapped->map_length);
        close(mapped->fd);
        mapped->map_base = NULL;
        mapped->fd       = -1;
        return -1;
    }

    mapped->page = (probe_page_t *)((char *)mapped->map_base + delta);
    return 0;
}

static void probe_unmap(mapped_probe_t *mapped)
{
    if (mapped->map_base != NULL) {
        munmap(mapped->map_base, mapped->map_length);
    }
    if (mapped->fd >= 0) {
        close(mapped->fd);
    }
}

static void probe_clear(mapped_probe_t *mapped, const probe_args_t *args)
{
    mapped->page->lock_word = 0;
    mapped->page->head_word = 0;
    mapped->page->faa_word  = 0;
    mapped->page->reserved  = 0;
    probe_bus_full_fence();
    printf("probe clear: dev=%s offset=%" PRIu64 "\n",
           args->dev_path, args->offset);
}

static void probe_log_lock_anomaly(const char *kind, uint64_t iter,
                                   uint64_t observed, uint64_t token,
                                   uint64_t head, uint64_t limit,
                                   uint64_t *count_p)
{
    if (*count_p >= limit) {
        return;
    }

    ++(*count_p);
    printf("lock64 anomaly[%s]: iter=%" PRIu64 " observed=0x%016" PRIx64
           " token=0x%016" PRIx64 " head=%" PRIu64 "\n",
           kind, iter, observed, token, head);
}

static void probe_run_lock64(mapped_probe_t *mapped, const probe_args_t *args)
{
    uint64_t i;
    uint64_t lock_busy = 0;
    uint64_t acquired = 0;
    uint64_t cas_readback_mismatch = 0;
    uint64_t owner_mismatch = 0;
    uint64_t readback_nonzero = 0;
    uint64_t anomaly_lines = 0;

    printf("lock64 start: dev=%s offset=%" PRIu64 " token=0x%016" PRIx64
           " iters=%" PRIu64 " hold_loops=%" PRIu64 "\n",
           args->dev_path, args->offset, args->token, args->iters,
           args->hold_loops);

    for (i = 0; i < args->iters; ++i) {
        uint64_t head;
        uint64_t observed;
        uint64_t spin;
        uint64_t old;

        old = probe_atomic_cswap64(&mapped->page->lock_word, 0, args->token);
        probe_bus_full_fence();
        if (old != 0) {
            ++lock_busy;
            continue;
        }

        observed = mapped->page->lock_word;
        if (observed != args->token) {
            ++cas_readback_mismatch;
            probe_log_lock_anomaly("cas_succeeded_readback_mismatch", i,
                                   observed, args->token,
                                   mapped->page->head_word,
                                   args->anomaly_limit, &anomaly_lines);
            probe_bus_store_fence();
            mapped->page->lock_word = 0;
            probe_bus_full_fence();
            continue;
        }

        ++acquired;

        probe_bus_load_fence();
        head = mapped->page->head_word;
        probe_bus_full_fence();
        mapped->page->head_word = head + 1;

        for (spin = 0; spin < args->hold_loops; ++spin) {
            __asm__ __volatile__("" ::: "memory");
        }

        probe_bus_load_fence();
        observed = mapped->page->lock_word;
        if (observed != args->token) {
            ++owner_mismatch;
            probe_log_lock_anomaly("owner_mismatch", i, observed, args->token,
                                   head, args->anomaly_limit, &anomaly_lines);
        }

        probe_bus_store_fence();
        mapped->page->lock_word = 0;
        probe_bus_full_fence();
        observed = mapped->page->lock_word;
        if (observed != 0) {
            ++readback_nonzero;
            probe_log_lock_anomaly("unlock_readback_nonzero", i, observed,
                                   args->token, head, args->anomaly_limit,
                                   &anomaly_lines);
        }
    }

    printf("lock64 summary: token=0x%016" PRIx64 " acquired=%" PRIu64
           " busy=%" PRIu64 " cas_readback_mismatch=%" PRIu64
           " owner_mismatch=%" PRIu64
           " unlock_readback_nonzero=%" PRIu64
           " final_lock=0x%016" PRIx64 " final_head=%" PRIu64 "\n",
           args->token, acquired, lock_busy, cas_readback_mismatch,
           owner_mismatch,
           readback_nonzero, mapped->page->lock_word, mapped->page->head_word);
}

static void probe_log_faa_anomaly(const char *kind, uint64_t iter, uint64_t oldv,
                                  uint64_t after, uint64_t prev_old,
                                  uint64_t limit, uint64_t *count_p)
{
    if (*count_p >= limit) {
        return;
    }

    ++(*count_p);
    printf("faa64 anomaly[%s]: iter=%" PRIu64 " old=%" PRIu64
           " after=%" PRIu64 " prev_old=%" PRIu64 "\n",
           kind, iter, oldv, after, prev_old);
}

static void probe_run_faa64(mapped_probe_t *mapped, const probe_args_t *args)
{
    uint64_t i;
    uint64_t oldv;
    uint64_t after;
    uint64_t prev_old = 0;
    uint64_t monotonic_fail = 0;
    uint64_t readback_fail = 0;
    uint64_t anomaly_lines = 0;
    uint64_t min_delta = UINT64_MAX;
    uint64_t max_delta = 0;
    int have_prev = 0;

    printf("faa64 start: dev=%s offset=%" PRIu64 " iters=%" PRIu64 "\n",
           args->dev_path, args->offset, args->iters);

    for (i = 0; i < args->iters; ++i) {
        oldv = probe_atomic_fetch_add64(&mapped->page->faa_word, 1);
        probe_bus_full_fence();
        after = mapped->page->faa_word;

        if (have_prev) {
            uint64_t delta = oldv - prev_old;

            if (oldv <= prev_old) {
                ++monotonic_fail;
                probe_log_faa_anomaly("non_monotonic_old", i, oldv, after,
                                      prev_old, args->anomaly_limit,
                                      &anomaly_lines);
            } else {
                if (delta < min_delta) {
                    min_delta = delta;
                }
                if (delta > max_delta) {
                    max_delta = delta;
                }
            }
        } else {
            have_prev = 1;
        }

        if (after < (oldv + 1)) {
            ++readback_fail;
            probe_log_faa_anomaly("readback_lt_old_plus_1", i, oldv, after,
                                  prev_old, args->anomaly_limit,
                                  &anomaly_lines);
        }

        prev_old = oldv;
    }

    if (min_delta == UINT64_MAX) {
        min_delta = 0;
    }

    printf("faa64 summary: ops=%" PRIu64 " monotonic_fail=%" PRIu64
           " readback_fail=%" PRIu64 " min_delta=%" PRIu64
           " max_delta=%" PRIu64 " final_counter=%" PRIu64 "\n",
           args->iters, monotonic_fail, readback_fail, min_delta, max_delta,
           mapped->page->faa_word);
}

int main(int argc, char **argv)
{
    probe_args_t  args;
    mapped_probe_t mapped;

    if (probe_parse_args(argc, argv, &args) != 0) {
        probe_usage(argv[0]);
        return 1;
    }

    if (probe_map(&args, &mapped) != 0) {
        return 1;
    }

    printf("probe atomic backend: %s\n", probe_atomic_backend_name());

    switch (args.mode) {
    case PROBE_MODE_CLEAR:
        probe_clear(&mapped, &args);
        break;
    case PROBE_MODE_LOCK64:
        probe_run_lock64(&mapped, &args);
        break;
    case PROBE_MODE_FAA64:
        probe_run_faa64(&mapped, &args);
        break;
    default:
        probe_usage(argv[0]);
        probe_unmap(&mapped);
        return 1;
    }

    probe_unmap(&mapped);
    return 0;
}
