/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 *
 * Standalone probe for comparing:
 *   1. a cacheable (CC) ownership-flip protocol that uses disjoint pub/ack/data
 *      pages, and
 *   2. the same pub/ack/data state machine over a non-cacheable (NC) mapping.
 *
 * Intended usage on two hosts with one process per host:
 *
 *   Host B:
 *     ./obmm_cc_nc_probe --role b --mode cc --memid <import-memid>
 *
 *   Host A:
 *     ./obmm_cc_nc_probe --role a --mode cc --memid <export-memid>
 *
 * Re-run with --mode nc on both sides to compare against the NC baseline.
 *
 * Each side passes the local shmdev memid that maps the SAME underlying region.
 * The probe uses 3 consecutive pages starting at --base-page:
 *   page 0: pub
 *   page 1: ack
 *   page 2: data
 *
 * Compile on a Linux host with:
 *   gcc -O3 -Wall -Wextra -std=gnu11 -o obmm_cc_nc_probe cc_nc_probe.c -ldl
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define OBMM_SYSFS_ROOT    "/sys/devices/obmm"
#define OBMM_DEV_PATH_FMT  "/dev/obmm_shmdev%llu"
#define OBMM_PATH_MAX      256

typedef enum probe_mode {
    PROBE_MODE_CC = 0,
    PROBE_MODE_NC = 1
} probe_mode_t;

typedef enum probe_role {
    PROBE_ROLE_A = 0,
    PROBE_ROLE_B = 1
} probe_role_t;

typedef int (*obmm_set_ownership_func_t)(int fd, void *start, void *end, int prot);

typedef struct probe_opts {
    probe_role_t role;
    probe_mode_t mode;
    uint64_t     memid;
    uint64_t     iterations;
    uint64_t     payload_bytes;
    uint64_t     timeout_sec;
    uint64_t     base_page;
    bool         reset;
    const char  *libobmm_path;
} probe_opts_t;

typedef struct probe_stats {
    uint64_t *samples_ns;
    uint64_t  total_ns;
    uint64_t  min_ns;
    uint64_t  max_ns;
    uint64_t  ownership_calls;
    uint64_t  ownership_ns;
    uint64_t  poll_loops;
    uint64_t  count;
} probe_stats_t;

typedef struct probe_ctx {
    probe_opts_t               opts;
    int                        fd;
    void                      *map_base;
    size_t                     map_len;
    size_t                     page_size;
    uint64_t                   region_size;
    volatile uint64_t         *pub_page;
    volatile uint64_t         *ack_page;
    volatile uint8_t          *data_page;
    obmm_set_ownership_func_t  set_ownership;
} probe_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --role a|b --mode cc|nc --memid <id> [options]\n"
            "\n"
            "Options:\n"
            "  --iters <n>          Number of messages (default: 100)\n"
            "  --payload-bytes <n>  Data bytes written in the data page (default: 8)\n"
            "  --timeout-sec <n>    Per-wait timeout in seconds (default: 30)\n"
            "  --base-page <n>      First page index inside the shmdev mapping (default: 0)\n"
            "  --no-reset           Role A skips zeroing pub/ack/data before start\n"
            "                       (requires pages to already be in the initial zero state)\n"
            "  --libobmm-path <p>   Path to libobmm.so for CC mode if not on loader path\n"
            "  -h, --help           Show this help\n",
            prog);
}

static void probe_bus_full_fence(void)
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

static void probe_cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    sched_yield();
#endif
}

static uint64_t now_ns(void)
{
    struct timespec ts;

#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

static uint64_t percentile_u64(const uint64_t *sorted, uint64_t count,
                               uint64_t num, uint64_t den)
{
    uint64_t idx;

    if (count == 0) {
        return 0;
    }

    idx = ((count - 1) * num) / den;
    return sorted[idx];
}

static void stats_init(probe_stats_t *stats, uint64_t count)
{
    memset(stats, 0, sizeof(*stats));
    stats->samples_ns = calloc((size_t)count, sizeof(*stats->samples_ns));
    if (stats->samples_ns == NULL) {
        fprintf(stderr, "failed to allocate %" PRIu64 " latency samples\n", count);
        exit(EXIT_FAILURE);
    }
    stats->min_ns = UINT64_MAX;
}

static void stats_record_sample(probe_stats_t *stats, uint64_t ns)
{
    stats->samples_ns[stats->count++] = ns;
    stats->total_ns += ns;
    if (ns < stats->min_ns) {
        stats->min_ns = ns;
    }
    if (ns > stats->max_ns) {
        stats->max_ns = ns;
    }
}

static void stats_print(const probe_ctx_t *ctx, probe_stats_t *stats,
                        const char *metric_name)
{
    double   avg_ns;
    double   rate_msgps;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    uint64_t own_avg;

    qsort(stats->samples_ns, (size_t)stats->count, sizeof(*stats->samples_ns),
          cmp_u64);
    p50 = percentile_u64(stats->samples_ns, stats->count, 50, 100);
    p95 = percentile_u64(stats->samples_ns, stats->count, 95, 100);
    p99 = percentile_u64(stats->samples_ns, stats->count, 99, 100);

    avg_ns    = (stats->count == 0) ? 0.0 :
                ((double)stats->total_ns / (double)stats->count);
    rate_msgps = (stats->total_ns == 0) ? 0.0 :
                 ((double)stats->count * 1e9) / (double)stats->total_ns;
    own_avg    = (stats->ownership_calls == 0) ? 0 :
                 (stats->ownership_ns / stats->ownership_calls);

    printf("summary "
           "mode=%s role=%s memid=%" PRIu64 " base_page=%" PRIu64
           " iters=%" PRIu64 " payload_bytes=%" PRIu64
           " %s_min_ns=%" PRIu64 " %s_p50_ns=%" PRIu64
           " %s_p95_ns=%" PRIu64 " %s_p99_ns=%" PRIu64
           " %s_avg_ns=%.2f %s_max_ns=%" PRIu64
           " rate_msgps=%.2f poll_loops=%" PRIu64
           " ownership_calls=%" PRIu64 " ownership_total_ns=%" PRIu64
           " ownership_avg_ns=%" PRIu64 "\n",
           (ctx->opts.mode == PROBE_MODE_CC) ? "cc" : "nc",
           (ctx->opts.role == PROBE_ROLE_A) ? "a" : "b",
           ctx->opts.memid, ctx->opts.base_page, ctx->opts.iterations,
           ctx->opts.payload_bytes,
           metric_name, stats->min_ns,
           metric_name, p50,
           metric_name, p95,
           metric_name, p99,
           metric_name, avg_ns,
           metric_name, stats->max_ns,
           rate_msgps, stats->poll_loops,
           stats->ownership_calls, stats->ownership_ns, own_avg);
}

static uint64_t parse_u64(const char *name, const char *value)
{
    char    *end;
    uint64_t parsed;

    errno  = 0;
    parsed = strtoull(value, &end, 0);
    if ((errno != 0) || (end == value) || (*end != '\0')) {
        fprintf(stderr, "invalid %s: '%s'\n", name, value);
        exit(EXIT_FAILURE);
    }

    return parsed;
}

static probe_role_t parse_role(const char *value)
{
    if (!strcasecmp(value, "a") || !strcasecmp(value, "sender")) {
        return PROBE_ROLE_A;
    }
    if (!strcasecmp(value, "b") || !strcasecmp(value, "receiver")) {
        return PROBE_ROLE_B;
    }

    fprintf(stderr, "invalid role: '%s'\n", value);
    exit(EXIT_FAILURE);
}

static probe_mode_t parse_mode(const char *value)
{
    if (!strcasecmp(value, "cc")) {
        return PROBE_MODE_CC;
    }
    if (!strcasecmp(value, "nc")) {
        return PROBE_MODE_NC;
    }

    fprintf(stderr, "invalid mode: '%s'\n", value);
    exit(EXIT_FAILURE);
}

static void parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    static const struct option long_opts[] = {
        {"role",          required_argument, NULL, 'r'},
        {"mode",          required_argument, NULL, 'm'},
        {"memid",         required_argument, NULL, 'i'},
        {"iters",         required_argument, NULL, 'n'},
        {"payload-bytes", required_argument, NULL, 'p'},
        {"timeout-sec",   required_argument, NULL, 't'},
        {"base-page",     required_argument, NULL, 'b'},
        {"no-reset",      no_argument,       NULL, 'R'},
        {"libobmm-path",  required_argument, NULL, 'l'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int  opt;
    bool have_role = false;
    bool have_mode = false;

    memset(opts, 0, sizeof(*opts));
    opts->iterations    = 100;
    opts->payload_bytes = sizeof(uint64_t);
    opts->timeout_sec   = 30;
    opts->reset         = true;

    while ((opt = getopt_long(argc, argv, "r:m:i:n:p:t:b:Rl:h", long_opts,
                              NULL)) != -1) {
        switch (opt) {
        case 'r':
            opts->role = parse_role(optarg);
            have_role  = true;
            break;
        case 'm':
            opts->mode = parse_mode(optarg);
            have_mode  = true;
            break;
        case 'i':
            opts->memid = parse_u64("memid", optarg);
            break;
        case 'n':
            opts->iterations = parse_u64("iters", optarg);
            break;
        case 'p':
            opts->payload_bytes = parse_u64("payload-bytes", optarg);
            break;
        case 't':
            opts->timeout_sec = parse_u64("timeout-sec", optarg);
            break;
        case 'b':
            opts->base_page = parse_u64("base-page", optarg);
            break;
        case 'R':
            opts->reset = false;
            break;
        case 'l':
            opts->libobmm_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (opts->memid == 0) {
        fprintf(stderr, "--memid must be provided and non-zero\n");
        exit(EXIT_FAILURE);
    }
    if (!have_role) {
        fprintf(stderr, "--role must be provided\n");
        exit(EXIT_FAILURE);
    }
    if (!have_mode) {
        fprintf(stderr, "--mode must be provided\n");
        exit(EXIT_FAILURE);
    }
    if (opts->iterations == 0) {
        fprintf(stderr, "--iters must be > 0\n");
        exit(EXIT_FAILURE);
    }
    if (opts->payload_bytes < sizeof(uint64_t)) {
        fprintf(stderr, "--payload-bytes must be >= %zu so the sequence number fits\n",
                sizeof(uint64_t));
        exit(EXIT_FAILURE);
    }
}

static void build_path(char *path, size_t path_len, uint64_t memid,
                       const char *suffix)
{
    int ret;

    if (suffix == NULL) {
        ret = snprintf(path, path_len, OBMM_DEV_PATH_FMT,
                       (unsigned long long)memid);
    } else {
        ret = snprintf(path, path_len, "%s/obmm_shmdev%llu/%s",
                       OBMM_SYSFS_ROOT, (unsigned long long)memid, suffix);
    }

    if ((ret < 0) || ((size_t)ret >= path_len)) {
        fprintf(stderr, "path construction failed for memid=%" PRIu64 "\n", memid);
        exit(EXIT_FAILURE);
    }
}

static uint64_t read_sysfs_hex_u64(uint64_t memid, const char *suffix)
{
    char     path[OBMM_PATH_MAX];
    char     buf[64];
    FILE    *fp;
    uint64_t value;

    build_path(path, sizeof(path), memid, suffix);
    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fprintf(stderr, "failed to read %s\n", path);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    fclose(fp);
    if (sscanf(buf, "%" SCNx64, &value) != 1) {
        fprintf(stderr, "failed to parse hex u64 from %s: '%s'\n", path, buf);
        exit(EXIT_FAILURE);
    }

    return value;
}

static long read_sysfs_long(uint64_t memid, const char *suffix)
{
    char path[OBMM_PATH_MAX];
    char buf[64];
    FILE *fp;
    long  value;
    char *end;

    build_path(path, sizeof(path), memid, suffix);
    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fprintf(stderr, "failed to read %s\n", path);
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    fclose(fp);

    errno = 0;
    value = strtol(buf, &end, 0);
    if ((errno != 0) || (end == buf)) {
        fprintf(stderr, "failed to parse long from %s: '%s'\n", path, buf);
        exit(EXIT_FAILURE);
    }

    return value;
}

static obmm_set_ownership_func_t resolve_set_ownership(const char *explicit_path)
{
    static const char *fallbacks[] = {"libobmm.so", "libobmm.so.0"};
    obmm_set_ownership_func_t func;
    void *handle;
    const char *env_path;
    size_t i;

    func = (obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT, "obmm_set_ownership");
    if (func != NULL) {
        return func;
    }

    if ((explicit_path != NULL) && (*explicit_path != '\0')) {
        handle = dlopen(explicit_path, RTLD_LAZY | RTLD_LOCAL);
        if (handle != NULL) {
            func = (obmm_set_ownership_func_t)dlsym(handle, "obmm_set_ownership");
            if (func != NULL) {
                return func;
            }
        }
    }

    env_path = getenv("OBMM_LIBOBMM_PATH");
    if ((env_path != NULL) && (*env_path != '\0')) {
        handle = dlopen(env_path, RTLD_LAZY | RTLD_LOCAL);
        if (handle != NULL) {
            func = (obmm_set_ownership_func_t)dlsym(handle, "obmm_set_ownership");
            if (func != NULL) {
                return func;
            }
        }
    }

    for (i = 0; i < (sizeof(fallbacks) / sizeof(fallbacks[0])); ++i) {
        handle = dlopen(fallbacks[i], RTLD_LAZY | RTLD_LOCAL);
        if (handle == NULL) {
            continue;
        }
        func = (obmm_set_ownership_func_t)dlsym(handle, "obmm_set_ownership");
        if (func != NULL) {
            return func;
        }
    }

    fprintf(stderr, "failed to resolve obmm_set_ownership; set --libobmm-path or "
            "OBMM_LIBOBMM_PATH\n");
    exit(EXIT_FAILURE);
}

static void open_mapping(probe_ctx_t *ctx)
{
    char   dev_path[OBMM_PATH_MAX];
    int    open_flags;
    int    prot;
    off_t  map_offset;

    ctx->page_size   = (size_t)sysconf(_SC_PAGESIZE);
    if (ctx->page_size == (size_t)-1) {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    ctx->region_size = read_sysfs_hex_u64(ctx->opts.memid, "size");

    if (read_sysfs_long(ctx->opts.memid, "allow_mmap") == 0) {
        fprintf(stderr, "memid=%" PRIu64 " does not allow mmap\n",
                ctx->opts.memid);
        exit(EXIT_FAILURE);
    }

    if (ctx->opts.payload_bytes > ctx->page_size) {
        fprintf(stderr, "payload_bytes=%" PRIu64 " exceeds page size=%zu\n",
                ctx->opts.payload_bytes, ctx->page_size);
        exit(EXIT_FAILURE);
    }

    ctx->map_len = 3 * ctx->page_size;
    map_offset   = (off_t)(ctx->opts.base_page * ctx->page_size);

    if (((uint64_t)map_offset + ctx->map_len) > ctx->region_size) {
        fprintf(stderr,
                "requested pages [%" PRIu64 ", %" PRIu64 ") exceed shmdev size=0x%"
                PRIx64 "\n",
                ctx->opts.base_page, ctx->opts.base_page + 3, ctx->region_size);
        exit(EXIT_FAILURE);
    }

    if (ctx->opts.mode == PROBE_MODE_NC) {
        open_flags = O_RDWR | O_SYNC | O_CLOEXEC;
        prot       = PROT_READ | PROT_WRITE;
    } else {
        open_flags = O_RDWR | O_CLOEXEC;
        prot       = PROT_NONE;
        ctx->set_ownership = resolve_set_ownership(ctx->opts.libobmm_path);
    }

    build_path(dev_path, sizeof(dev_path), ctx->opts.memid, NULL);
    ctx->fd = open(dev_path, open_flags);
    if (ctx->fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    ctx->map_base = mmap(NULL, ctx->map_len, prot, MAP_SHARED, ctx->fd, map_offset);
    if (ctx->map_base == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, len=%zu, offset=%lld) failed: %s\n",
                dev_path, ctx->map_len, (long long)map_offset, strerror(errno));
        close(ctx->fd);
        exit(EXIT_FAILURE);
    }

    ctx->pub_page  = (volatile uint64_t*)ctx->map_base;
    ctx->ack_page  = (volatile uint64_t*)((char*)ctx->map_base + ctx->page_size);
    ctx->data_page = (volatile uint8_t*)((char*)ctx->map_base + 2 * ctx->page_size);
}

static void close_mapping(probe_ctx_t *ctx)
{
    if ((ctx->map_base != NULL) && (ctx->map_base != MAP_FAILED)) {
        munmap(ctx->map_base, ctx->map_len);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
}

static void set_ownership_or_die(probe_ctx_t *ctx, probe_stats_t *stats,
                                 void *start, size_t length, int prot)
{
    uint64_t t0;
    uint64_t t1;

    t0 = now_ns();
    if (ctx->set_ownership(ctx->fd, start, (char*)start + length, prot) != 0) {
        fprintf(stderr,
                "obmm_set_ownership(memid=%" PRIu64 ", start=%p, len=%zu, prot=%d) "
                "failed: %s\n",
                ctx->opts.memid, start, length, prot, strerror(errno));
        exit(EXIT_FAILURE);
    }
    t1 = now_ns();

    stats->ownership_calls++;
    stats->ownership_ns += (t1 - t0);
}

static void fill_payload(volatile uint8_t *data, uint64_t payload_bytes,
                         uint64_t seq)
{
    uint64_t i;

    *(volatile uint64_t*)data = seq;
    for (i = sizeof(uint64_t); i < payload_bytes; ++i) {
        data[i] = (uint8_t)((seq + i) & 0xffu);
    }
}

static void verify_payload(const volatile uint8_t *data, uint64_t payload_bytes,
                           uint64_t expected_seq)
{
    uint64_t seq;
    uint64_t i;
    uint8_t  expected;

    seq = *(const volatile uint64_t*)data;
    if (seq != expected_seq) {
        fprintf(stderr, "data mismatch: expected seq=%" PRIu64 " got=%" PRIu64 "\n",
                expected_seq, seq);
        exit(EXIT_FAILURE);
    }

    for (i = sizeof(uint64_t); i < payload_bytes; ++i) {
        expected = (uint8_t)((expected_seq + i) & 0xffu);
        if (data[i] != expected) {
            fprintf(stderr,
                    "payload byte mismatch at offset=%" PRIu64
                    " expected=0x%02x got=0x%02x\n",
                    i, expected, data[i]);
            exit(EXIT_FAILURE);
        }
    }
}

static void reset_pages(probe_ctx_t *ctx, probe_stats_t *stats)
{
    if (!ctx->opts.reset || (ctx->opts.role != PROBE_ROLE_A)) {
        return;
    }

    if (ctx->opts.mode == PROBE_MODE_CC) {
        set_ownership_or_die(ctx, stats, (void*)ctx->pub_page,
                             3 * ctx->page_size, PROT_WRITE);
        memset((void*)ctx->pub_page, 0, 3 * ctx->page_size);
        probe_bus_full_fence();
        set_ownership_or_die(ctx, stats, (void*)ctx->pub_page,
                             3 * ctx->page_size, PROT_NONE);
    } else {
        memset((void*)ctx->pub_page, 0, 3 * ctx->page_size);
        probe_bus_full_fence();
    }
}

static void read_pub_ack(probe_ctx_t *ctx, probe_stats_t *stats,
                         uint64_t *pub, uint64_t *ack)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        set_ownership_or_die(ctx, stats, (void*)ctx->pub_page,
                             2 * ctx->page_size, PROT_READ);
        probe_bus_full_fence();
        *pub = *ctx->pub_page;
        *ack = *ctx->ack_page;
        set_ownership_or_die(ctx, stats, (void*)ctx->pub_page,
                             2 * ctx->page_size, PROT_NONE);
    } else {
        probe_bus_full_fence();
        *pub = *ctx->pub_page;
        *ack = *ctx->ack_page;
    }
}

static void write_pub(probe_ctx_t *ctx, probe_stats_t *stats, uint64_t seq)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        set_ownership_or_die(ctx, stats, (void*)ctx->pub_page,
                             ctx->page_size, PROT_WRITE);
        *ctx->pub_page = seq;
        probe_bus_full_fence();
        set_ownership_or_die(ctx, stats, (void*)ctx->pub_page,
                             ctx->page_size, PROT_NONE);
    } else {
        *ctx->pub_page = seq;
        probe_bus_full_fence();
    }
}

static void write_ack(probe_ctx_t *ctx, probe_stats_t *stats, uint64_t seq)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        set_ownership_or_die(ctx, stats, (void*)ctx->ack_page,
                             ctx->page_size, PROT_WRITE);
        *ctx->ack_page = seq;
        probe_bus_full_fence();
        set_ownership_or_die(ctx, stats, (void*)ctx->ack_page,
                             ctx->page_size, PROT_NONE);
    } else {
        *ctx->ack_page = seq;
        probe_bus_full_fence();
    }
}

static void write_data(probe_ctx_t *ctx, probe_stats_t *stats, uint64_t seq)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        set_ownership_or_die(ctx, stats, (void*)ctx->data_page,
                             ctx->page_size, PROT_WRITE);
        fill_payload(ctx->data_page, ctx->opts.payload_bytes, seq);
        probe_bus_full_fence();
        set_ownership_or_die(ctx, stats, (void*)ctx->data_page,
                             ctx->page_size, PROT_NONE);
    } else {
        fill_payload(ctx->data_page, ctx->opts.payload_bytes, seq);
        probe_bus_full_fence();
    }
}

static void read_data(probe_ctx_t *ctx, probe_stats_t *stats, uint64_t expected_seq)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        set_ownership_or_die(ctx, stats, (void*)ctx->data_page,
                             ctx->page_size, PROT_READ);
        probe_bus_full_fence();
        verify_payload(ctx->data_page, ctx->opts.payload_bytes, expected_seq);
        set_ownership_or_die(ctx, stats, (void*)ctx->data_page,
                             ctx->page_size, PROT_NONE);
    } else {
        probe_bus_full_fence();
        verify_payload(ctx->data_page, ctx->opts.payload_bytes, expected_seq);
    }
}

static void timeout_die(const probe_ctx_t *ctx, const char *phase, uint64_t expected,
                        uint64_t pub, uint64_t ack)
{
    fprintf(stderr,
            "timeout phase=%s mode=%s role=%s expected=%" PRIu64
            " pub=%" PRIu64 " ack=%" PRIu64 "\n",
            phase,
            (ctx->opts.mode == PROBE_MODE_CC) ? "cc" : "nc",
            (ctx->opts.role == PROBE_ROLE_A) ? "a" : "b",
            expected, pub, ack);
    exit(EXIT_FAILURE);
}

static void wait_sender_ready(probe_ctx_t *ctx, probe_stats_t *stats,
                              uint64_t expected_prev)
{
    uint64_t deadline = now_ns() + (ctx->opts.timeout_sec * 1000000000ull);
    uint64_t pub;
    uint64_t ack;

    for (;;) {
        read_pub_ack(ctx, stats, &pub, &ack);
        if ((pub == expected_prev) && (ack == expected_prev)) {
            return;
        }

        stats->poll_loops++;
        if (now_ns() > deadline) {
            timeout_die(ctx, "sender_wait_ready", expected_prev, pub, ack);
        }
        probe_cpu_relax();
    }
}

static void wait_sender_ack(probe_ctx_t *ctx, probe_stats_t *stats, uint64_t expected)
{
    uint64_t deadline = now_ns() + (ctx->opts.timeout_sec * 1000000000ull);
    uint64_t pub;
    uint64_t ack;

    for (;;) {
        read_pub_ack(ctx, stats, &pub, &ack);
        if ((pub == expected) && (ack == expected)) {
            return;
        }

        stats->poll_loops++;
        if (now_ns() > deadline) {
            timeout_die(ctx, "sender_wait_ack", expected, pub, ack);
        }
        probe_cpu_relax();
    }
}

static void wait_receiver_message(probe_ctx_t *ctx, probe_stats_t *stats,
                                  uint64_t expected_seq)
{
    uint64_t deadline = now_ns() + (ctx->opts.timeout_sec * 1000000000ull);
    uint64_t pub;
    uint64_t ack;

    for (;;) {
        read_pub_ack(ctx, stats, &pub, &ack);
        if ((pub == expected_seq) && (ack == (expected_seq - 1))) {
            return;
        }
        if ((pub > expected_seq) || (ack >= expected_seq)) {
            fprintf(stderr,
                    "unexpected control state before read: expected_seq=%" PRIu64
                    " pub=%" PRIu64 " ack=%" PRIu64 "\n",
                    expected_seq, pub, ack);
            exit(EXIT_FAILURE);
        }

        stats->poll_loops++;
        if (now_ns() > deadline) {
            timeout_die(ctx, "receiver_wait_message", expected_seq, pub, ack);
        }
        probe_cpu_relax();
    }
}

static void run_role_a(probe_ctx_t *ctx, probe_stats_t *stats)
{
    uint64_t seq;
    uint64_t t0;
    uint64_t t1;

    reset_pages(ctx, stats);

    for (seq = 1; seq <= ctx->opts.iterations; ++seq) {
        wait_sender_ready(ctx, stats, seq - 1);
        t0 = now_ns();
        write_data(ctx, stats, seq);
        write_pub(ctx, stats, seq);
        wait_sender_ack(ctx, stats, seq);
        t1 = now_ns();
        stats_record_sample(stats, t1 - t0);
    }
}

static void run_role_b(probe_ctx_t *ctx, probe_stats_t *stats)
{
    uint64_t seq;
    uint64_t t0;
    uint64_t t1;

    for (seq = 1; seq <= ctx->opts.iterations; ++seq) {
        wait_receiver_message(ctx, stats, seq);
        t0 = now_ns();
        read_data(ctx, stats, seq);
        probe_bus_full_fence();
        write_ack(ctx, stats, seq);
        t1 = now_ns();
        stats_record_sample(stats, t1 - t0);
    }
}

int main(int argc, char **argv)
{
    probe_ctx_t   ctx;
    probe_stats_t stats;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;

    parse_opts(argc, argv, &ctx.opts);
    open_mapping(&ctx);
    stats_init(&stats, ctx.opts.iterations);

    if (ctx.opts.role == PROBE_ROLE_A) {
        run_role_a(&ctx, &stats);
        stats_print(&ctx, &stats, "rtt");
    } else {
        run_role_b(&ctx, &stats);
        stats_print(&ctx, &stats, "service");
    }

    close_mapping(&ctx);
    free(stats.samples_ns);
    return 0;
}
