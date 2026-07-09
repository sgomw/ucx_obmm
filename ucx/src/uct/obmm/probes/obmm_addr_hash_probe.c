/*
 * Standalone OBMM NC address-index contention probe.
 *
 * This probe is intentionally outside UCX. It maps one victim shmdev and one
 * or more aggressor shmdevs as NC, fixes the victim address, sweeps the
 * aggressor offset, and prints the measured victim slowdown together with PA
 * low-bit relationships from obmm_query_pa_by_memid().
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OBMM_SYSFS_ROOT "/sys/devices/obmm"
#define OBMM_DEV_FMT    "/dev/obmm_shmdev%" PRIu64
#define OBMM_2M         (2ull * 1024ull * 1024ull)
#define OBMM_1M         (1024ull * 1024ull)
#define OBMM_512K       (512ull * 1024ull)
#define OBMM_64K        (64ull * 1024ull)
#define OBMM_4K         (4ull * 1024ull)
#define OBMM_CACHELINE  64ull

typedef uint64_t obmm_mem_id_t;
typedef int (*obmm_query_pa_by_memid_func_t)(obmm_mem_id_t id,
                                             unsigned long offset,
                                             unsigned long *pa);

typedef enum {
    OP_READ64,
    OP_WRITE64,
    OP_RW64,
    OP_STORE_LOAD64
} probe_op_t;

typedef struct {
    uint64_t memid;
    uint64_t size;
    uint8_t *map;
} obmm_map_t;

typedef struct {
    uint64_t *values;
    size_t    count;
    size_t    capacity;
} memid_list_t;

typedef struct {
    const char *lib_path;
    uint64_t    victim_memid;
    uint64_t    victim_offset;
    uint64_t    scan_start;
    uint64_t    scan_step;
    unsigned    scan_count;
    double      seconds;
    probe_op_t  op;
    int         fence;
    int         baseline;
    int         query_pa;
    int         pin_base;
    memid_list_t aggressors;
} probe_opts_t;

typedef struct {
    pthread_barrier_t barrier;
    atomic_int        stop;
    atomic_int        terminate;
    atomic_int        aggressors_active;
    probe_op_t        op;
    int               fence;
} probe_shared_t;

typedef struct {
    probe_shared_t     *shared;
    volatile uint64_t  *ptr;
    uint64_t            ops;
    uint64_t            sink;
    int                 is_victim;
    int                 pin_cpu;
} worker_arg_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --victim-memid N --aggressor-memids LIST [options]\n"
            "\n"
            "Options:\n"
            "  --lib PATH              libobmm shared object path "
            "(default: libobmm.so)\n"
            "  --victim-offset N       fixed victim offset "
            "(default: 2MiB)\n"
            "  --scan-start N          first aggressor offset "
            "(default: 2MiB)\n"
            "  --scan-step N           aggressor offset step "
            "(default: 4KiB)\n"
            "  --scan-count N          number of scan points "
            "(default: 512)\n"
            "  --seconds S             seconds per point "
            "(default: 0.05)\n"
            "  --op read64|write64|rw64|store-load64 "
            "(default: rw64)\n"
            "  --fence                 issue a bus-domain fence per op\n"
            "  --no-baseline           skip victim-only baseline phase\n"
            "  --no-pa-query           run without obmm_query_pa_by_memid\n"
            "  --pin-base CPU          pin victim to CPU and aggressors after it\n"
            "  --help                  show this help\n"
            "\n"
            "LIST accepts comma-separated memids and ranges, for example "
            "2-70,75.\n",
            prog);
}

static int parse_u64(const char *s, uint64_t *value)
{
    char               *end = NULL;
    unsigned long long  v;

    errno = 0;
    v = strtoull(s, &end, 0);
    if ((errno != 0) || (end == s) || (*end != '\0')) {
        return 0;
    }

    *value = (uint64_t)v;
    return 1;
}

static int parse_uint(const char *s, unsigned *value)
{
    uint64_t v;

    if (!parse_u64(s, &v) || (v > UINT_MAX)) {
        return 0;
    }

    *value = (unsigned)v;
    return 1;
}

static int parse_double(const char *s, double *value)
{
    char  *end = NULL;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if ((errno != 0) || (end == s) || (*end != '\0') || (v <= 0.0)) {
        return 0;
    }

    *value = v;
    return 1;
}

static int parse_op(const char *s, probe_op_t *op)
{
    if (!strcmp(s, "read64")) {
        *op = OP_READ64;
        return 1;
    } else if (!strcmp(s, "write64")) {
        *op = OP_WRITE64;
        return 1;
    } else if (!strcmp(s, "rw64")) {
        *op = OP_RW64;
        return 1;
    } else if (!strcmp(s, "store-load64")) {
        *op = OP_STORE_LOAD64;
        return 1;
    }

    return 0;
}

static const char *op_name(probe_op_t op)
{
    switch (op) {
    case OP_READ64:
        return "read64";
    case OP_WRITE64:
        return "write64";
    case OP_RW64:
        return "rw64";
    case OP_STORE_LOAD64:
        return "store-load64";
    default:
        return "unknown";
    }
}

static int memid_list_append(memid_list_t *list, uint64_t value)
{
    uint64_t *values;
    size_t    capacity;

    if (value == 0) {
        return 0;
    }

    if (list->count == list->capacity) {
        capacity = (list->capacity == 0) ? 16 : (list->capacity * 2);
        values = realloc(list->values, capacity * sizeof(*values));
        if (values == NULL) {
            return 0;
        }
        list->values   = values;
        list->capacity = capacity;
    }

    list->values[list->count++] = value;
    return 1;
}

static int parse_memid_list(const char *s, memid_list_t *list)
{
    const char *p = s;

    while (*p != '\0') {
        char token[64];
        const char *comma;
        const char *dash;
        size_t len;
        uint64_t first, last, v;

        comma = strchr(p, ',');
        len = (comma == NULL) ? strlen(p) : (size_t)(comma - p);
        if ((len == 0) || (len >= sizeof(token))) {
            return 0;
        }

        memcpy(token, p, len);
        token[len] = '\0';
        dash = strchr(token, '-');
        if (dash == NULL) {
            if (!parse_u64(token, &first) || !memid_list_append(list, first)) {
                return 0;
            }
        } else {
            char left[64];
            char right[64];
            size_t left_len = (size_t)(dash - token);

            if ((left_len == 0) || (left_len >= sizeof(left))) {
                return 0;
            }
            if (strlen(dash + 1) >= sizeof(right)) {
                return 0;
            }

            memcpy(left, token, left_len);
            left[left_len] = '\0';
            strcpy(right, dash + 1);

            if (!parse_u64(left, &first) || !parse_u64(right, &last) ||
                (first > last)) {
                return 0;
            }
            for (v = first; v <= last; ++v) {
                if (!memid_list_append(list, v)) {
                    return 0;
                }
                if (v == UINT64_MAX) {
                    return 0;
                }
            }
        }

        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }

    return list->count > 0;
}

static int parse_args(int argc, char **argv, probe_opts_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->lib_path      = "libobmm.so";
    opts->victim_offset = OBMM_2M;
    opts->scan_start    = OBMM_2M;
    opts->scan_step     = OBMM_4K;
    opts->scan_count    = 512;
    opts->seconds       = 0.05;
    opts->op            = OP_RW64;
    opts->baseline      = 1;
    opts->query_pa      = 1;
    opts->pin_base      = -1;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "--lib") && (++i < argc)) {
            opts->lib_path = argv[i];
        } else if (!strcmp(argv[i], "--victim-memid") && (++i < argc)) {
            if (!parse_u64(argv[i], &opts->victim_memid)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--aggressor-memids") && (++i < argc)) {
            if (!parse_memid_list(argv[i], &opts->aggressors)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--victim-offset") && (++i < argc)) {
            if (!parse_u64(argv[i], &opts->victim_offset)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--scan-start") && (++i < argc)) {
            if (!parse_u64(argv[i], &opts->scan_start)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--scan-step") && (++i < argc)) {
            if (!parse_u64(argv[i], &opts->scan_step)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--scan-count") && (++i < argc)) {
            if (!parse_uint(argv[i], &opts->scan_count)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--seconds") && (++i < argc)) {
            if (!parse_double(argv[i], &opts->seconds)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--op") && (++i < argc)) {
            if (!parse_op(argv[i], &opts->op)) {
                return 0;
            }
        } else if (!strcmp(argv[i], "--fence")) {
            opts->fence = 1;
        } else if (!strcmp(argv[i], "--no-baseline")) {
            opts->baseline = 0;
        } else if (!strcmp(argv[i], "--no-pa-query")) {
            opts->query_pa = 0;
        } else if (!strcmp(argv[i], "--pin-base") && (++i < argc)) {
            uint64_t v;

            if (!parse_u64(argv[i], &v) || (v > INT_MAX)) {
                return 0;
            }
            opts->pin_base = (int)v;
        } else {
            return 0;
        }
    }

    return (opts->victim_memid != 0) && (opts->aggressors.count > 0) &&
           (opts->scan_count > 0) && (opts->scan_step > 0) &&
           ((opts->victim_offset % sizeof(uint64_t)) == 0) &&
           ((opts->scan_start % sizeof(uint64_t)) == 0) &&
           ((opts->scan_step % sizeof(uint64_t)) == 0);
}

static uint64_t mask(uint64_t align)
{
    return align - 1u;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
}

static void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void nc_full_barrier(void)
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

static void passive_wait_until_stop(probe_shared_t *shared)
{
    struct timespec req = {0, 100000};

    while (!atomic_load_explicit(&shared->stop, memory_order_acquire)) {
        nanosleep(&req, NULL);
    }
}

static uint64_t probe_one(volatile uint64_t *ptr, probe_op_t op,
                          uint64_t seq, int fence)
{
    uint64_t v;

    switch (op) {
    case OP_READ64:
        v = *ptr;
        break;
    case OP_WRITE64:
        *ptr = seq;
        v = seq;
        break;
    case OP_RW64:
        v = *ptr;
        *ptr = v + 1u;
        break;
    case OP_STORE_LOAD64:
        *ptr = seq;
        v = *ptr;
        break;
    default:
        v = 0;
        break;
    }

    if (fence) {
        nc_full_barrier();
    }

    return v;
}

static void try_pin_thread(int cpu)
{
#if defined(__linux__)
    cpu_set_t set;

    if (cpu < 0) {
        return;
    }
    if (cpu >= CPU_SETSIZE) {
        return;
    }

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

static void *worker_main(void *arg)
{
    worker_arg_t   *worker = arg;
    probe_shared_t *shared = worker->shared;

    try_pin_thread(worker->pin_cpu);

    for (;;) {
        uint64_t seq = ((uint64_t)(uintptr_t)worker << 8) | 1u;
        uint64_t ops = 0;
        uint64_t sink = 0;
        int      active;

        pthread_barrier_wait(&shared->barrier);
        if (atomic_load_explicit(&shared->terminate, memory_order_acquire)) {
            break;
        }

        active = worker->is_victim ||
                 atomic_load_explicit(&shared->aggressors_active,
                                      memory_order_acquire);
        if (active) {
            while (!atomic_load_explicit(&shared->stop, memory_order_acquire)) {
                sink += probe_one(worker->ptr, shared->op, seq++,
                                  shared->fence);
                ++ops;
            }
        } else {
            passive_wait_until_stop(shared);
        }

        worker->ops  = ops;
        worker->sink = sink;
        pthread_barrier_wait(&shared->barrier);
    }

    return NULL;
}

static int read_region_size(uint64_t memid, uint64_t *size_p)
{
    char     path[PATH_MAX];
    char     buf[64];
    FILE    *f;
    uint64_t size;

    snprintf(path, sizeof(path), "%s/obmm_shmdev%" PRIu64 "/size",
             OBMM_SYSFS_ROOT, memid);

    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return 0;
    }

    if (fgets(buf, sizeof(buf), f) == NULL) {
        fprintf(stderr, "failed to read %s\n", path);
        fclose(f);
        return 0;
    }

    fclose(f);

    if (sscanf(buf, "%" SCNx64, &size) != 1) {
        fprintf(stderr, "failed to parse hex size from %s: %s\n", path, buf);
        return 0;
    }

    *size_p = size;
    return 1;
}

static int open_map(uint64_t memid, obmm_map_t *map)
{
    char dev_path[PATH_MAX];
    int  fd;

    memset(map, 0, sizeof(*map));
    map->memid = memid;

    if (!read_region_size(memid, &map->size)) {
        return 0;
    }
    if (map->size > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "memid=%" PRIu64 " too large to mmap: %" PRIu64 "\n",
                memid, map->size);
        return 0;
    }

    snprintf(dev_path, sizeof(dev_path), OBMM_DEV_FMT, memid);
    fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev_path, strerror(errno));
        return 0;
    }

    map->map = mmap(NULL, (size_t)map->size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    if (map->map == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, size=%" PRIu64 ") failed: %s\n",
                dev_path, map->size, strerror(errno));
        map->map = NULL;
        close(fd);
        return 0;
    }

    close(fd);
    return 1;
}

static void close_map(obmm_map_t *map)
{
    if (map->map != NULL) {
        munmap(map->map, (size_t)map->size);
        map->map = NULL;
    }
}

static int check_offset(const obmm_map_t *map, uint64_t offset)
{
    if ((offset > map->size) ||
        (sizeof(uint64_t) > (map->size - offset))) {
        fprintf(stderr, "memid=%" PRIu64 " too small for offset=%" PRIu64
                " size=%" PRIu64 "\n", map->memid, offset, map->size);
        return 0;
    }

    return 1;
}

static int query_pa(obmm_query_pa_by_memid_func_t query, uint64_t memid,
                    uint64_t offset, uint64_t *pa_p)
{
    unsigned long pa;

    errno = 0;
    if (query((obmm_mem_id_t)memid, (unsigned long)offset, &pa) != 0) {
        fprintf(stderr, "obmm_query_pa_by_memid(memid=%" PRIu64
                ", offset=%" PRIu64 ") failed: %s\n",
                memid, offset, strerror(errno));
        return 0;
    }

    *pa_p = (uint64_t)pa;
    return 1;
}

static double run_phase(probe_shared_t *shared, worker_arg_t *workers,
                        size_t num_workers, int aggressors_active,
                        double seconds)
{
    double t0, end, t1;
    size_t i;

    for (i = 0; i < num_workers; ++i) {
        workers[i].ops  = 0;
        workers[i].sink = 0;
    }

    atomic_store_explicit(&shared->stop, 0, memory_order_release);
    atomic_store_explicit(&shared->aggressors_active, aggressors_active,
                          memory_order_release);

    pthread_barrier_wait(&shared->barrier);
    t0  = now_sec();
    end = t0 + seconds;
    while (now_sec() < end) {
        cpu_relax();
    }
    atomic_store_explicit(&shared->stop, 1, memory_order_release);
    pthread_barrier_wait(&shared->barrier);
    t1 = now_sec();

    return t1 - t0;
}

static int count_low_matches(const uint64_t *aggr_pa, size_t count,
                             uint64_t victim_pa, uint64_t align)
{
    uint64_t m = mask(align);
    int      matches = 0;
    size_t   i;

    for (i = 0; i < count; ++i) {
        if (((aggr_pa[i] ^ victim_pa) & m) == 0) {
            ++matches;
        }
    }

    return matches;
}

static int load_query_func(const probe_opts_t *opts, void **lib_p,
                           obmm_query_pa_by_memid_func_t *query_p)
{
    void *lib;

    *lib_p   = NULL;
    *query_p = NULL;

    if (!opts->query_pa) {
        return 1;
    }

    lib = dlopen(opts->lib_path, RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", opts->lib_path, dlerror());
        return 0;
    }

    *query_p = (obmm_query_pa_by_memid_func_t)
               dlsym(lib, "obmm_query_pa_by_memid");
    if (*query_p == NULL) {
        fprintf(stderr, "dlsym(obmm_query_pa_by_memid) failed: %s\n",
                dlerror());
        dlclose(lib);
        return 0;
    }

    *lib_p = lib;
    return 1;
}

int main(int argc, char **argv)
{
    probe_opts_t opts;
    obmm_map_t victim;
    obmm_map_t *aggr_maps = NULL;
    uint64_t *aggr_pa = NULL;
    probe_shared_t shared;
    worker_arg_t *workers = NULL;
    pthread_t *threads = NULL;
    void *lib = NULL;
    obmm_query_pa_by_memid_func_t query = NULL;
    uint64_t victim_pa = 0;
    uint64_t max_scan_offset;
    double baseline_ops_s = 0.0;
    size_t num_workers;
    size_t i;
    int ret = 1;

    if (!parse_args(argc, argv, &opts)) {
        usage(argv[0]);
        return 2;
    }

    if (!load_query_func(&opts, &lib, &query)) {
        goto out_free_opts;
    }
    for (i = 0; i < opts.aggressors.count; ++i) {
        if (opts.aggressors.values[i] == opts.victim_memid) {
            fprintf(stderr, "victim memid must not be in aggressor list\n");
            goto out_close_lib;
        }
    }

    if (!open_map(opts.victim_memid, &victim)) {
        goto out_close_lib;
    }
    if (!check_offset(&victim, opts.victim_offset)) {
        goto out_close_victim;
    }

    if (opts.scan_count > 1) {
        uint64_t span = (uint64_t)(opts.scan_count - 1) * opts.scan_step;
        if ((opts.scan_step != 0) &&
            (span / opts.scan_step != (uint64_t)(opts.scan_count - 1))) {
            fprintf(stderr, "scan offset overflow\n");
            goto out_close_victim;
        }
        if (span > (UINT64_MAX - opts.scan_start)) {
            fprintf(stderr, "scan offset overflow\n");
            goto out_close_victim;
        }
        max_scan_offset = opts.scan_start + span;
    } else {
        max_scan_offset = opts.scan_start;
    }

    aggr_maps = calloc(opts.aggressors.count, sizeof(*aggr_maps));
    aggr_pa   = calloc(opts.aggressors.count, sizeof(*aggr_pa));
    if ((aggr_maps == NULL) || (aggr_pa == NULL)) {
        perror("calloc aggressor arrays");
        goto out_close_victim;
    }

    for (i = 0; i < opts.aggressors.count; ++i) {
        if (!open_map(opts.aggressors.values[i], &aggr_maps[i])) {
            goto out_close_aggr;
        }
        if (!check_offset(&aggr_maps[i], max_scan_offset)) {
            goto out_close_aggr;
        }
    }

    if (opts.query_pa) {
        if (!query_pa(query, opts.victim_memid, opts.victim_offset,
                      &victim_pa)) {
            goto out_close_aggr;
        }
    }

    num_workers = 1u + opts.aggressors.count;
    workers = calloc(num_workers, sizeof(*workers));
    threads = calloc(num_workers, sizeof(*threads));
    if ((workers == NULL) || (threads == NULL)) {
        perror("calloc workers");
        goto out_close_aggr;
    }

    memset(&shared, 0, sizeof(shared));
    shared.op    = opts.op;
    shared.fence = opts.fence;
    if (pthread_barrier_init(&shared.barrier, NULL,
                             (unsigned)(num_workers + 1u)) != 0) {
        perror("pthread_barrier_init");
        goto out_free_workers;
    }

    workers[0].shared    = &shared;
    workers[0].is_victim = 1;
    workers[0].pin_cpu   = opts.pin_base;
    workers[0].ptr       = (volatile uint64_t*)
                           (victim.map + opts.victim_offset);
    for (i = 0; i < opts.aggressors.count; ++i) {
        workers[i + 1u].shared    = &shared;
        workers[i + 1u].is_victim = 0;
        workers[i + 1u].pin_cpu   = (opts.pin_base < 0) ?
                                    -1 : (opts.pin_base + 1 + (int)i);
        workers[i + 1u].ptr       = (volatile uint64_t*)
                                    (aggr_maps[i].map + opts.scan_start);
    }

    for (i = 0; i < num_workers; ++i) {
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    printf("OBMM_ADDR_HASH_PROBE_CONFIG victim_memid=%" PRIu64
           " victim_offset=%" PRIu64 " query_pa=%d victim_pa=0x%" PRIx64
           " victim_pa_low2m=0x%" PRIx64 " aggressors=%zu scan_start=%" PRIu64
           " scan_step=%" PRIu64 " scan_count=%u op=%s fence=%d seconds=%.6f\n",
           opts.victim_memid, opts.victim_offset, opts.query_pa, victim_pa,
           opts.query_pa ? (victim_pa & mask(OBMM_2M)) : 0,
           opts.aggressors.count, opts.scan_start, opts.scan_step,
           opts.scan_count, op_name(opts.op), opts.fence, opts.seconds);

    if (opts.baseline) {
        double elapsed = run_phase(&shared, workers, num_workers, 0,
                                   opts.seconds);
        baseline_ops_s = (double)workers[0].ops / elapsed;
        printf("OBMM_ADDR_HASH_BASELINE victim_memid=%" PRIu64
               " victim_offset=%" PRIu64 " victim_pa=0x%" PRIx64
               " victim_ops=%" PRIu64 " elapsed=%.9f ops_per_sec=%.3f"
               " ns_per_op=%.3f sink=%" PRIu64 "\n",
               opts.victim_memid, opts.victim_offset, victim_pa,
               workers[0].ops, elapsed, baseline_ops_s,
               (baseline_ops_s > 0.0) ? (1e9 / baseline_ops_s) : 0.0,
               workers[0].sink);
        fflush(stdout);
    }

    for (i = 0; i < opts.scan_count; ++i) {
        uint64_t aggr_offset = opts.scan_start + ((uint64_t)i *
                               opts.scan_step);
        uint64_t aggr_ops = 0;
        double elapsed, victim_ops_s, aggr_ops_s, slowdown;
        size_t j;
        int match64 = -1, match4k = -1, match64k = -1;
        int match512k = -1, match1m = -1, match2m = -1;
        uint64_t first_aggr_pa = 0;
        uint64_t first_xor_low2m = 0;

        for (j = 0; j < opts.aggressors.count; ++j) {
            workers[j + 1u].ptr = (volatile uint64_t*)
                                  (aggr_maps[j].map + aggr_offset);
            if (opts.query_pa &&
                !query_pa(query, aggr_maps[j].memid, aggr_offset,
                          &aggr_pa[j])) {
                goto out_stop_threads;
            }
        }

        if (opts.query_pa) {
            first_aggr_pa   = aggr_pa[0];
            first_xor_low2m = (first_aggr_pa ^ victim_pa) & mask(OBMM_2M);
            match64   = count_low_matches(aggr_pa, opts.aggressors.count,
                                          victim_pa, OBMM_CACHELINE);
            match4k   = count_low_matches(aggr_pa, opts.aggressors.count,
                                          victim_pa, OBMM_4K);
            match64k  = count_low_matches(aggr_pa, opts.aggressors.count,
                                          victim_pa, OBMM_64K);
            match512k = count_low_matches(aggr_pa, opts.aggressors.count,
                                          victim_pa, OBMM_512K);
            match1m   = count_low_matches(aggr_pa, opts.aggressors.count,
                                          victim_pa, OBMM_1M);
            match2m   = count_low_matches(aggr_pa, opts.aggressors.count,
                                          victim_pa, OBMM_2M);
        }

        elapsed = run_phase(&shared, workers, num_workers, 1, opts.seconds);
        for (j = 0; j < opts.aggressors.count; ++j) {
            aggr_ops += workers[j + 1u].ops;
        }

        victim_ops_s = (double)workers[0].ops / elapsed;
        aggr_ops_s   = (double)aggr_ops / elapsed;
        slowdown     = (baseline_ops_s > 0.0) ?
                       (baseline_ops_s / victim_ops_s) : 0.0;

        printf("OBMM_ADDR_HASH_STEP step=%zu victim_memid=%" PRIu64
               " victim_offset=%" PRIu64 " victim_pa=0x%" PRIx64
               " victim_pa_low2m=0x%" PRIx64 " aggr_offset=%" PRIu64
               " first_aggr_memid=%" PRIu64 " first_aggr_pa=0x%" PRIx64
               " first_aggr_pa_low2m=0x%" PRIx64
               " first_xor_low2m=0x%" PRIx64
               " match64=%d match4k=%d match64k=%d match512k=%d"
               " match1m=%d match2m=%d victim_ops=%" PRIu64
               " aggr_ops=%" PRIu64 " elapsed=%.9f"
               " victim_ops_per_sec=%.3f aggr_ops_per_sec=%.3f"
               " victim_ns_per_op=%.3f slowdown=%.6f sink=%" PRIu64 "\n",
               i, opts.victim_memid, opts.victim_offset, victim_pa,
               opts.query_pa ? (victim_pa & mask(OBMM_2M)) : 0,
               aggr_offset, aggr_maps[0].memid, first_aggr_pa,
               opts.query_pa ? (first_aggr_pa & mask(OBMM_2M)) : 0,
               first_xor_low2m, match64, match4k, match64k, match512k,
               match1m, match2m, workers[0].ops, aggr_ops, elapsed,
               victim_ops_s, aggr_ops_s,
               (victim_ops_s > 0.0) ? (1e9 / victim_ops_s) : 0.0,
               slowdown, workers[0].sink);
        fflush(stdout);
    }

    ret = 0;

out_stop_threads:
    atomic_store_explicit(&shared.terminate, 1, memory_order_release);
    pthread_barrier_wait(&shared.barrier);
out_join_threads:
    for (i = 0; i < num_workers; ++i) {
        if (threads[i] != 0) {
            pthread_join(threads[i], NULL);
        }
    }
    pthread_barrier_destroy(&shared.barrier);
out_free_workers:
    free(threads);
    free(workers);
out_close_aggr:
    if (aggr_maps != NULL) {
        for (i = 0; i < opts.aggressors.count; ++i) {
            close_map(&aggr_maps[i]);
        }
    }
    free(aggr_pa);
    free(aggr_maps);
out_close_victim:
    close_map(&victim);
out_close_lib:
    if (lib != NULL) {
        dlclose(lib);
    }
out_free_opts:
    free(opts.aggressors.values);
    return ret;
}
