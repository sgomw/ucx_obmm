/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 *
 * OBMM CC-vs-NC latency probe — v3.
 *
 * Compares three access disciplines for the same single-page seq/ack
 * ping-pong protocol:
 *
 *   nc            Non-cacheable  (O_SYNC, PROT_READ|PROT_WRITE, no syscalls)
 *   cc-ownership  Cacheable, mmap(PROT_NONE) once, obmm_set_ownership flips
 *   cc-mmap       Cacheable, dynamic mmap/munmap each access (4K granularity)
 *
 * Page layout (same for all modes):
 *   [seq: uint64_t][ack: uint64_t][payload: payload_bytes]
 *
 *   seq  — written by A, read by B
 *   ack  — written by B, read by A
 *
 * Intended usage on two hosts:
 *
 *   Host B:
 *     ./obmm_cc_nc_probe_v3 --role b --mode nc|cc-ownership|cc-mmap --memid <id>
 *   Host A:
 *     ./obmm_cc_nc_probe_v3 --role a --mode nc|cc-ownership|cc-mmap --memid <id>
 *
 * Compile:
 *   gcc -O3 -Wall -Wextra -std=gnu11 -o obmm_cc_nc_probe_v3 cc_nc_probe_v3.c -ldl
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
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define OBMM_SYSFS_ROOT   "/sys/devices/obmm"
#define OBMM_DEV_PATH_FMT "/dev/obmm_shmdev%llu"
#define OBMM_PATH_MAX     256

/* ====================================================================
 * types
 * ==================================================================== */

typedef enum {
    PROBE_MODE_NC = 0,
    PROBE_MODE_CC_OWNERSHIP,
    PROBE_MODE_CC_MMAP
} probe_mode_t;

typedef enum { PROBE_ROLE_A = 0, PROBE_ROLE_B = 1 } probe_role_t;

typedef int (*obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                         int prot);

typedef struct {
    probe_role_t role;
    probe_mode_t mode;
    uint64_t     memid;
    uint64_t     iterations;
    uint64_t     payload_bytes;
    uint64_t     timeout_sec;
    uint64_t     base_page;
    int          poll_backoff_us;
    bool         reset;
    const char  *libobmm_path;
} probe_opts_t;

typedef struct {
    uint64_t *samples_ns;
    uint64_t  total_ns;
    uint64_t  min_ns;
    uint64_t  max_ns;
    uint64_t  access_syscalls;
    uint64_t  access_syscall_ns;
    uint64_t  poll_loops;
    uint64_t  count;
} probe_stats_t;

typedef struct {
    probe_opts_t               opts;
    int                        fd;
    size_t                     page_size;
    size_t                     map_len;
    off_t                      map_off;
    char                       dev_path[OBMM_PATH_MAX];
    /* nc / cc-ownership: persistent mapping at fixed_base.
     * cc-mmap:          fixed_base is NULL; map/unmap each access.  */
    void                      *fixed_base;
    obmm_set_ownership_func_t  set_ownership;    /* cc-ownership only   */
    /* pointers valid only during an access window */
    volatile uint64_t         *seq_p;
    volatile uint64_t         *ack_p;
    volatile uint8_t          *payload_p;
    uint64_t                   poll_backoff_ns;
} probe_ctx_t;

/* ====================================================================
 * small helpers
 * ==================================================================== */

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
    if (count == 0) return 0;
    return sorted[((count - 1) * num) / den];
}

static void stats_init(probe_stats_t *stats, uint64_t count)
{
    memset(stats, 0, sizeof(*stats));
    stats->samples_ns = calloc((size_t)count, sizeof(*stats->samples_ns));
    if (stats->samples_ns == NULL) {
        fprintf(stderr, "failed to allocate %" PRIu64 " samples\n", count);
        exit(EXIT_FAILURE);
    }
    stats->min_ns = UINT64_MAX;
}

static void stats_record(probe_stats_t *stats, uint64_t ns)
{
    stats->samples_ns[stats->count++] = ns;
    stats->total_ns += ns;
    if (ns < stats->min_ns) stats->min_ns = ns;
    if (ns > stats->max_ns) stats->max_ns = ns;
}

static const char *mode_name(probe_mode_t m)
{
    switch (m) {
    case PROBE_MODE_NC:           return "nc";
    case PROBE_MODE_CC_OWNERSHIP: return "cc-ownership";
    case PROBE_MODE_CC_MMAP:      return "cc-mmap";
    default:                      return "?";
    }
}

static void stats_print(const probe_ctx_t *ctx, const probe_stats_t *s,
                        const char *metric)
{
    qsort(s->samples_ns, (size_t)s->count, sizeof(*s->samples_ns), cmp_u64);
    double avg_ns = (s->count == 0) ? 0.0
                   : ((double)s->total_ns / (double)s->count);
    double rate   = (s->total_ns == 0) ? 0.0
                   : ((double)s->count * 1e9) / (double)s->total_ns;
    uint64_t sys_avg = (s->access_syscalls == 0) ? 0
                     : (s->access_syscall_ns / s->access_syscalls);

    printf("summary "
           "mode=%s role=%s memid=%" PRIu64 " base_page=%" PRIu64
           " iters=%" PRIu64 " payload_bytes=%" PRIu64
           " %s_min_ns=%" PRIu64 " %s_p50_ns=%" PRIu64
           " %s_p95_ns=%" PRIu64 " %s_p99_ns=%" PRIu64
           " %s_avg_ns=%.2f %s_max_ns=%" PRIu64
           " rate_msgps=%.2f poll_loops=%" PRIu64
           " access_syscalls=%" PRIu64 " access_syscall_total_ns=%" PRIu64
           " access_syscall_avg_ns=%" PRIu64 "\n",
           mode_name(ctx->opts.mode),
           (ctx->opts.role == PROBE_ROLE_A) ? "a" : "b",
           ctx->opts.memid, ctx->opts.base_page,
           ctx->opts.iterations, ctx->opts.payload_bytes,
           metric, s->min_ns, metric,
           percentile_u64(s->samples_ns, s->count, 50, 100),
           metric, percentile_u64(s->samples_ns, s->count, 95, 100),
           metric, percentile_u64(s->samples_ns, s->count, 99, 100),
           metric, avg_ns, metric, s->max_ns,
           rate, s->poll_loops,
           s->access_syscalls, s->access_syscall_ns, sys_avg);
}

/* ====================================================================
 * arg parsing
 * ==================================================================== */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --role a|b --mode nc|cc-ownership|cc-mmap --memid <id> [opts]\n"
        "\n"
        "Modes:\n"
        "  nc             Non-cacheable  (O_SYNC, no access syscalls)\n"
        "  cc-ownership   Cacheable, mmap(PROT_NONE) + obmm_set_ownership flips\n"
        "  cc-mmap        Cacheable, dynamic mmap/munmap each access\n"
        "\n"
        "Options:\n"
        "  --iters <n>           Messages (default: 100)\n"
        "  --payload-bytes <n>   Payload bytes (default: 8, max: page-16)\n"
        "  --timeout-sec <n>     Timeout in seconds (default: 30)\n"
        "  --base-page <n>       Page index inside shmdev (default: 0)\n"
        "  --poll-backoff-us <n> Poll backoff in us (default: 10)\n"
        "  --no-reset            Skip zeroing the page before start\n"
        "  --libobmm-path <p>    Path to libobmm.so (cc-ownership only)\n"
        "  -h, --help            Show this help\n",
        prog);
}

static uint64_t parse_u64(const char *name, const char *value)
{
    char *end;
    errno = 0;
    uint64_t v = strtoull(value, &end, 0);
    if ((errno != 0) || (end == value) || (*end != '\0')) {
        fprintf(stderr, "invalid %s: '%s'\n", name, value);
        exit(EXIT_FAILURE);
    }
    return v;
}

static probe_role_t parse_role(const char *s)
{
    if (!strcasecmp(s, "a") || !strcasecmp(s, "sender"))  return PROBE_ROLE_A;
    if (!strcasecmp(s, "b") || !strcasecmp(s, "receiver")) return PROBE_ROLE_B;
    fprintf(stderr, "invalid role: '%s'\n", s);
    exit(EXIT_FAILURE);
}

static probe_mode_t parse_mode(const char *s)
{
    if (!strcasecmp(s, "nc"))            return PROBE_MODE_NC;
    if (!strcasecmp(s, "cc-ownership"))  return PROBE_MODE_CC_OWNERSHIP;
    if (!strcasecmp(s, "cc-mmap"))       return PROBE_MODE_CC_MMAP;
    fprintf(stderr, "invalid mode: '%s'\n", s);
    exit(EXIT_FAILURE);
}

static void parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    static const struct option long_opts[] = {
        {"role",           required_argument, NULL, 'r'},
        {"mode",           required_argument, NULL, 'm'},
        {"memid",          required_argument, NULL, 'i'},
        {"iters",          required_argument, NULL, 'n'},
        {"payload-bytes",  required_argument, NULL, 'p'},
        {"timeout-sec",    required_argument, NULL, 't'},
        {"base-page",      required_argument, NULL, 'b'},
        {"poll-backoff-us",required_argument, NULL, 'B'},
        {"no-reset",       no_argument,       NULL, 'R'},
        {"libobmm-path",   required_argument, NULL, 'l'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    memset(opts, 0, sizeof(*opts));
    opts->iterations      = 100;
    opts->payload_bytes   = sizeof(uint64_t);
    opts->timeout_sec     = 30;
    opts->poll_backoff_us = 10;
    opts->reset           = true;

    while ((opt = getopt_long(argc, argv, "r:m:i:n:p:t:b:B:Rl:h",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r': opts->role = parse_role(optarg);  break;
        case 'm': opts->mode = parse_mode(optarg);  break;
        case 'i': opts->memid = parse_u64("memid", optarg); break;
        case 'n': opts->iterations = parse_u64("iters", optarg); break;
        case 'p': opts->payload_bytes = parse_u64("payload-bytes", optarg); break;
        case 't': opts->timeout_sec = parse_u64("timeout-sec", optarg); break;
        case 'b': opts->base_page = parse_u64("base-page", optarg); break;
        case 'B': opts->poll_backoff_us = (int)parse_u64("poll-backoff-us", optarg); break;
        case 'R': opts->reset = false;              break;
        case 'l': opts->libobmm_path = optarg;      break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default:  usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (opts->memid == 0) { fprintf(stderr, "--memid required\n"); exit(EXIT_FAILURE); }
    if (opts->iterations == 0) { fprintf(stderr, "--iters > 0\n"); exit(EXIT_FAILURE); }
    if (opts->payload_bytes < sizeof(uint64_t)) {
        fprintf(stderr, "--payload-bytes >= %zu\n", sizeof(uint64_t));
        exit(EXIT_FAILURE);
    }
}

/* ====================================================================
 * sysfs
 * ==================================================================== */

static void build_dev_path(char *path, size_t len, uint64_t memid)
{
    if ((size_t)snprintf(path, len, OBMM_DEV_PATH_FMT,
                         (unsigned long long)memid) >= len) {
        fprintf(stderr, "device path too long\n");
        exit(EXIT_FAILURE);
    }
}

static uint64_t read_sysfs_hex_u64(uint64_t memid, const char *attr)
{
    char path[OBMM_PATH_MAX], buf[64];
    snprintf(path, sizeof(path), "%s/obmm_shmdev%llu/%s",
             OBMM_SYSFS_ROOT, (unsigned long long)memid, attr);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(EXIT_FAILURE); }
    if (fgets(buf, sizeof(buf), fp) == NULL) { fclose(fp); fprintf(stderr, "read %s\n", path); exit(EXIT_FAILURE); }
    fclose(fp);
    uint64_t v;
    if (sscanf(buf, "%" SCNx64, &v) != 1) { fprintf(stderr, "parse %s\n", path); exit(EXIT_FAILURE); }
    return v;
}

static long read_sysfs_long(uint64_t memid, const char *attr)
{
    char path[OBMM_PATH_MAX], buf[64];
    snprintf(path, sizeof(path), "%s/obmm_shmdev%llu/%s",
             OBMM_SYSFS_ROOT, (unsigned long long)memid, attr);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(EXIT_FAILURE); }
    if (fgets(buf, sizeof(buf), fp) == NULL) { fclose(fp); fprintf(stderr, "read %s\n", path); exit(EXIT_FAILURE); }
    fclose(fp);
    char *end; errno = 0;
    long v = strtol(buf, &end, 0);
    if ((errno != 0) || (end == buf)) { fprintf(stderr, "parse %s\n", path); exit(EXIT_FAILURE); }
    return v;
}

/* ====================================================================
 * libobmm resolution (cc-ownership only)
 * ==================================================================== */

static obmm_set_ownership_func_t
resolve_set_ownership(const char *explicit_path)
{
    static const char *fallbacks[] = {"libobmm.so", "libobmm.so.0"};
    obmm_set_ownership_func_t f;
    f = (obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT, "obmm_set_ownership");
    if (f != NULL) return f;
    if (explicit_path != NULL && *explicit_path != '\0') {
        void *h = dlopen(explicit_path, RTLD_LAZY | RTLD_LOCAL);
        if (h != NULL) { f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership"); if (f) return f; }
    }
    const char *env = getenv("OBMM_LIBOBMM_PATH");
    if (env != NULL && *env != '\0') {
        void *h = dlopen(env, RTLD_LAZY | RTLD_LOCAL);
        if (h != NULL) { f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership"); if (f) return f; }
    }
    for (size_t i = 0; i < sizeof(fallbacks)/sizeof(fallbacks[0]); i++) {
        void *h = dlopen(fallbacks[i], RTLD_LAZY | RTLD_LOCAL);
        if (h == NULL) continue;
        f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership");
        if (f != NULL) return f;
    }
    fprintf(stderr, "cannot resolve obmm_set_ownership\n");
    exit(EXIT_FAILURE);
}

/* ====================================================================
 * setup / teardown
 * ==================================================================== */

static void setup(probe_ctx_t *ctx)
{
    ctx->page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (ctx->page_size == (size_t)-1) { perror("sysconf"); exit(EXIT_FAILURE); }
    ctx->region_size = read_sysfs_hex_u64(ctx->opts.memid, "size");
    if (read_sysfs_long(ctx->opts.memid, "allow_mmap") == 0) {
        fprintf(stderr, "memid=%" PRIu64 " does not allow mmap\n", ctx->opts.memid);
        exit(EXIT_FAILURE);
    }
    if (ctx->opts.payload_bytes + 2 * sizeof(uint64_t) > ctx->page_size) {
        fprintf(stderr, "payload+header > page_size\n");
        exit(EXIT_FAILURE);
    }
    ctx->map_len = ctx->page_size;
    ctx->map_off = (off_t)(ctx->opts.base_page * ctx->page_size);
    if ((uint64_t)(ctx->map_off + (off_t)ctx->map_len) > ctx->region_size) {
        fprintf(stderr, "page out of range\n");
        exit(EXIT_FAILURE);
    }
    build_dev_path(ctx->dev_path, sizeof(ctx->dev_path), ctx->opts.memid);

    int fl = (ctx->opts.mode == PROBE_MODE_NC) ? (O_RDWR | O_SYNC | O_CLOEXEC)
                                                : (O_RDWR | O_CLOEXEC);
    ctx->fd = open(ctx->dev_path, fl);
    if (ctx->fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); exit(EXIT_FAILURE); }

    if (ctx->opts.mode == PROBE_MODE_NC) {
        ctx->fixed_base = mmap(NULL, ctx->map_len, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->fd, ctx->map_off);
    } else if (ctx->opts.mode == PROBE_MODE_CC_OWNERSHIP) {
        ctx->set_ownership = resolve_set_ownership(ctx->opts.libobmm_path);
        ctx->fixed_base = mmap(NULL, ctx->map_len, PROT_NONE,
                               MAP_SHARED, ctx->fd, ctx->map_off);
    } else {
        ctx->fixed_base = NULL;  /* cc-mmap: no persistent mapping */
    }
    if (ctx->fixed_base != NULL && ctx->fixed_base == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno)); exit(EXIT_FAILURE);
    }

    ctx->poll_backoff_ns = (uint64_t)ctx->opts.poll_backoff_us * 1000ull;
}

static void teardown(probe_ctx_t *ctx)
{
    if (ctx->fixed_base != NULL && ctx->fixed_base != MAP_FAILED)
        munmap(ctx->fixed_base, ctx->map_len);
    if (ctx->fd >= 0) close(ctx->fd);
}

/* ====================================================================
 * access discipline layer — three implementations of the same contract
 *
 * Each access function ensures the caller can read/write the page,
 * fences appropriately, and releases access when done.
 *
 *   nc:           fence only, no syscalls (persistent RW mapping)
 *   cc-ownership: set_ownership(PROT_x) ... set_ownership(PROT_NONE)
 *   cc-mmap:      mmap(PROT_x) ... munmap
 *
 * The pointers ctx->seq_p / ctx->ack_p / ctx->payload_p are valid
 * ONLY between the acquire and release calls.
 * ==================================================================== */

/* -- field pointers --------------------------------------------------- */

/* For nc/cc-ownership modes the pointers are at fixed offsets from
 * fixed_base.  For cc-mmap they are set by mmap_page(). */
static void set_pointers_fixed(probe_ctx_t *ctx)
{
    ctx->seq_p     = (volatile uint64_t*)ctx->fixed_base;
    ctx->ack_p     = (volatile uint64_t*)ctx->fixed_base + 1;
    ctx->payload_p = (volatile uint8_t*) ctx->fixed_base + 2 * sizeof(uint64_t);
}

/* -- payload helpers (work on current pointers) ----------------------- */

static void fill_payload(const probe_ctx_t *ctx, uint64_t seq)
{
    *(volatile uint64_t*)ctx->payload_p = seq;
    for (uint64_t i = sizeof(uint64_t); i < ctx->opts.payload_bytes; i++)
        ctx->payload_p[i] = (uint8_t)((seq + i) & 0xffu);
}

static void verify_payload(const probe_ctx_t *ctx, uint64_t exp_seq)
{
    uint64_t seq = *(const volatile uint64_t*)ctx->payload_p;
    if (seq != exp_seq) {
        fprintf(stderr, "seq mismatch: expected=%" PRIu64 " got=%" PRIu64 "\n",
                exp_seq, seq);
        exit(EXIT_FAILURE);
    }
    for (uint64_t i = sizeof(uint64_t); i < ctx->opts.payload_bytes; i++) {
        uint8_t exp = (uint8_t)((exp_seq + i) & 0xffu);
        if (ctx->payload_p[i] != exp) {
            fprintf(stderr, "payload[%" PRIu64 "]: expected=0x%02x got=0x%02x\n",
                    i, exp, ctx->payload_p[i]);
            exit(EXIT_FAILURE);
        }
    }
}

/* -- cc-ownership flip ------------------------------------------------ */

static void ownership_flip(probe_ctx_t *ctx, probe_stats_t *s, int prot)
{
    uint64_t t0 = now_ns();
    int rc = ctx->set_ownership(ctx->fd, ctx->fixed_base,
                                (char*)ctx->fixed_base + ctx->map_len, prot);
    uint64_t t1 = now_ns();
    s->access_syscalls++;
    s->access_syscall_ns += (t1 - t0);
    if (rc != 0) {
        fprintf(stderr, "obmm_set_ownership(prot=%d): %s\n", prot, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* -- cc-mmap map / unmap ---------------------------------------------- */

static void mmap_page(probe_ctx_t *ctx, probe_stats_t *s, int prot)
{
    uint64_t t0 = now_ns();
    void *p = mmap(NULL, ctx->map_len, prot, MAP_SHARED, ctx->fd, ctx->map_off);
    uint64_t t1 = now_ns();
    s->access_syscalls++;
    s->access_syscall_ns += (t1 - t0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap(prot=%d): %s\n", prot, strerror(errno));
        exit(EXIT_FAILURE);
    }
    ctx->seq_p     = (volatile uint64_t*)p;
    ctx->ack_p     = (volatile uint64_t*)p + 1;
    ctx->payload_p = (volatile uint8_t*) p + 2 * sizeof(uint64_t);
}

static void munmap_page(probe_ctx_t *ctx, probe_stats_t *s)
{
    uint64_t t0 = now_ns();
    int rc = munmap((void*)ctx->seq_p, ctx->map_len);
    uint64_t t1 = now_ns();
    s->access_syscalls++;
    s->access_syscall_ns += (t1 - t0);
    if (rc != 0) { fprintf(stderr, "munmap: %s\n", strerror(errno)); exit(EXIT_FAILURE); }
    ctx->seq_p = ctx->ack_p = NULL;
    ctx->payload_p = NULL;
}

/* -- unified acquire / release wrappers ------------------------------- */

/* Read a single uint64_t field (seq or ack). */
static uint64_t read_field(probe_ctx_t *ctx, probe_stats_t *s,
                           volatile uint64_t *fn(probe_ctx_t*))
{
    uint64_t v;
    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        probe_bus_full_fence();
        v = *fn(ctx);
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        ownership_flip(ctx, s, PROT_READ);
        probe_bus_full_fence();
        v = *fn(ctx);
        ownership_flip(ctx, s, PROT_NONE);
        break;
    case PROBE_MODE_CC_MMAP:
        mmap_page(ctx, s, PROT_READ);
        probe_bus_full_fence();
        v = *fn(ctx);
        munmap_page(ctx, s);
        break;
    }
    return v;
}

/* Write seq + payload.  A-only — does not touch ack. */
static void write_msg(probe_ctx_t *ctx, probe_stats_t *s, uint64_t seq)
{
    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        *ctx->seq_p = seq;
        fill_payload(ctx, seq);
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        ownership_flip(ctx, s, PROT_WRITE);
        *ctx->seq_p = seq;
        fill_payload(ctx, seq);
        probe_bus_full_fence();
        ownership_flip(ctx, s, PROT_NONE);
        break;
    case PROBE_MODE_CC_MMAP:
        mmap_page(ctx, s, PROT_READ | PROT_WRITE);
        *ctx->seq_p = seq;
        fill_payload(ctx, seq);
        probe_bus_full_fence();
        munmap_page(ctx, s);
        break;
    }
}

/* B's turn: verify payload, then write ack = seq. */
static void read_and_ack(probe_ctx_t *ctx, probe_stats_t *s, uint64_t seq)
{
    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        probe_bus_full_fence();
        verify_payload(ctx, seq);
        *ctx->ack_p = seq;
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        ownership_flip(ctx, s, PROT_READ);
        probe_bus_full_fence();
        verify_payload(ctx, seq);
        ownership_flip(ctx, s, PROT_WRITE);
        *ctx->ack_p = seq;
        probe_bus_full_fence();
        ownership_flip(ctx, s, PROT_NONE);
        break;
    case PROBE_MODE_CC_MMAP:
        mmap_page(ctx, s, PROT_READ);
        probe_bus_full_fence();
        verify_payload(ctx, seq);
        munmap_page(ctx, s);
        mmap_page(ctx, s, PROT_READ | PROT_WRITE);
        *ctx->ack_p = seq;
        probe_bus_full_fence();
        munmap_page(ctx, s);
        break;
    }
}

/* -- field accessor functions (passed to read_field) ------------------ */

static volatile uint64_t *get_seq(probe_ctx_t *ctx) { return ctx->seq_p; }
static volatile uint64_t *get_ack(probe_ctx_t *ctx) { return ctx->ack_p; }

/* ====================================================================
 * protocol
 * ==================================================================== */

static void poll_backoff(const probe_ctx_t *ctx)
{
    if (ctx->poll_backoff_ns > 0) {
        uint64_t dl = now_ns() + ctx->poll_backoff_ns;
        while (now_ns() < dl) probe_cpu_relax();
    }
}

static void timeout_die(const probe_ctx_t *ctx, const char *phase,
                        uint64_t expected, uint64_t got)
{
    fprintf(stderr, "timeout phase=%s mode=%s role=%s expected=%" PRIu64
            " got=%" PRIu64 "\n",
            phase, mode_name(ctx->opts.mode),
            (ctx->opts.role == PROBE_ROLE_A) ? "a" : "b", expected, got);
    exit(EXIT_FAILURE);
}

static void wait_field(probe_ctx_t *ctx, probe_stats_t *s,
                       volatile uint64_t *fn(probe_ctx_t*),
                       uint64_t expected, const char *phase)
{
    uint64_t dl = now_ns() + (ctx->opts.timeout_sec * 1000000000ull);
    for (;;) {
        if (read_field(ctx, s, fn) == expected) return;
        s->poll_loops++;
        if (now_ns() > dl) timeout_die(ctx, phase, expected, read_field(ctx, s, fn));
        poll_backoff(ctx);
    }
}

/* ====================================================================
 * roles
 * ==================================================================== */

static void reset_page(probe_ctx_t *ctx, probe_stats_t *s)
{
    if (!ctx->opts.reset || ctx->opts.role != PROBE_ROLE_A) return;

    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        memset(ctx->fixed_base, 0, ctx->map_len);
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        ownership_flip(ctx, s, PROT_WRITE);
        memset(ctx->fixed_base, 0, ctx->map_len);
        probe_bus_full_fence();
        ownership_flip(ctx, s, PROT_NONE);
        break;
    case PROBE_MODE_CC_MMAP:
        mmap_page(ctx, s, PROT_READ | PROT_WRITE);
        memset((void*)ctx->seq_p, 0, ctx->map_len);
        probe_bus_full_fence();
        munmap_page(ctx, s);
        break;
    }
}

static void run_role_a(probe_ctx_t *ctx, probe_stats_t *s)
{
    if (ctx->opts.mode != PROBE_MODE_CC_MMAP) set_pointers_fixed(ctx);
    reset_page(ctx, s);

    for (uint64_t seq = 1; seq <= ctx->opts.iterations; seq++) {
        wait_field(ctx, s, get_ack, seq - 1, "A_wait_ack_prev");
        uint64_t t0 = now_ns();
        write_msg(ctx, s, seq);
        wait_field(ctx, s, get_ack, seq, "A_wait_ack");
        stats_record(s, now_ns() - t0);
    }
}

static void run_role_b(probe_ctx_t *ctx, probe_stats_t *s)
{
    if (ctx->opts.mode != PROBE_MODE_CC_MMAP) set_pointers_fixed(ctx);

    for (uint64_t seq = 1; seq <= ctx->opts.iterations; seq++) {
        wait_field(ctx, s, get_seq, seq, "B_wait_msg");
        uint64_t t0 = now_ns();
        read_and_ack(ctx, s, seq);
        stats_record(s, now_ns() - t0);
    }
}

/* ====================================================================
 * main
 * ==================================================================== */

int main(int argc, char **argv)
{
    probe_ctx_t   ctx;
    probe_stats_t stats;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;

    parse_opts(argc, argv, &ctx.opts);
    setup(&ctx);
    stats_init(&stats, ctx.opts.iterations);

    if (ctx.opts.role == PROBE_ROLE_A) run_role_a(&ctx, &stats);
    else                               run_role_b(&ctx, &stats);

    stats_print(&ctx, &stats,
                (ctx.opts.role == PROBE_ROLE_A) ? "rtt" : "service");
    teardown(&ctx);
    free(stats.samples_ns);
    return 0;
}
