/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 *
 * OBMM CC-vs-NC latency probe — v2.
 *
 * v1 (cc_nc_probe.c) violated the OBMM cacheable consistency model by
 * assuming per-page independent ownership, allowing Role A to hold
 * PROT_WRITE on the data page while Role B simultaneously held PROT_READ
 * on the pub/ack pages within the same mapped region.
 *
 * OBMM requires that when ANY host holds PROT_WRITE on a cacheable
 * mapping of a region, ALL other hosts must be PROT_NONE on that entire
 * region.  Conversely, when multiple hosts hold PROT_READ, NO host may
 * hold PROT_WRITE.
 *
 * v2 therefore uses a **single page** and fully serialised turn-taking:
 * only one host accesses the region at any moment.  The protocol is
 * identical for CC and NC — only the access mechanism differs
 * (set_ownership flips vs. bus fences).
 *
 * Intended usage on two hosts with one process per host:
 *
 *   Host B:
 *     ./obmm_cc_nc_probe_v2 --role b --mode cc --memid <import-memid>
 *
 *   Host A:
 *     ./obmm_cc_nc_probe_v2 --role a --mode cc --memid <export-memid>
 *
 * Compile on a Linux host with:
 *   gcc -O3 -Wall -Wextra -std=gnu11 -o obmm_cc_nc_probe_v2 cc_nc_probe_v2.c -ldl
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

/* --- types ----------------------------------------------------------- */

typedef enum { PROBE_MODE_CC = 0, PROBE_MODE_NC = 1 } probe_mode_t;
typedef enum { PROBE_ROLE_A = 0, PROBE_ROLE_B = 1 } probe_role_t;

typedef int (*obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                         int prot);

typedef struct {
    probe_role_t role;
    probe_mode_t mode;
    uint64_t     memid;
    uint64_t     iterations;
    uint64_t     payload_bytes;   /* must be < page_size - 8             */
    uint64_t     timeout_sec;
    uint64_t     base_page;
    int          poll_backoff_us; /* usleep between poll iterations      */
    bool         reset;
    const char  *libobmm_path;
} probe_opts_t;

typedef struct {
    uint64_t *samples_ns;
    uint64_t  total_ns;
    uint64_t  min_ns;
    uint64_t  max_ns;
    uint64_t  ownership_calls;
    uint64_t  ownership_total_ns;
    uint64_t  poll_loops;
    uint64_t  count;
} probe_stats_t;

typedef struct {
    probe_opts_t               opts;
    int                        fd;
    void                      *map_base;
    size_t                     page_size;
    uint64_t                   region_size;
    volatile uint64_t         *seq_p;         /* first 8 bytes of page   */
    volatile uint8_t          *payload_p;     /* remaining bytes          */
    obmm_set_ownership_func_t  set_ownership;
    uint64_t                   poll_backoff_ns;
} probe_ctx_t;

/* --- helpers --------------------------------------------------------- */

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

static void stats_print(const probe_ctx_t *ctx, const probe_stats_t *s,
                        const char *metric)
{
    qsort(s->samples_ns, (size_t)s->count, sizeof(*s->samples_ns), cmp_u64);
    double avg_ns = (s->count == 0) ? 0.0
                   : ((double)s->total_ns / (double)s->count);
    double rate   = (s->total_ns == 0) ? 0.0
                   : ((double)s->count * 1e9) / (double)s->total_ns;
    uint64_t own_avg = (s->ownership_calls == 0) ? 0
                     : (s->ownership_total_ns / s->ownership_calls);

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
           ctx->opts.memid, ctx->opts.base_page,
           ctx->opts.iterations, ctx->opts.payload_bytes,
           metric, s->min_ns, metric, percentile_u64(s->samples_ns, s->count, 50, 100),
           metric, percentile_u64(s->samples_ns, s->count, 95, 100),
           metric, percentile_u64(s->samples_ns, s->count, 99, 100),
           metric, avg_ns, metric, s->max_ns,
           rate, s->poll_loops,
           s->ownership_calls, s->ownership_total_ns, own_avg);
}

/* --- argument parsing ------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --role a|b --mode cc|nc --memid <id> [options]\n"
        "\n"
        "Options:\n"
        "  --iters <n>           Messages (default: 100)\n"
        "  --payload-bytes <n>   Payload bytes (default: 8, max: page-8)\n"
        "  --timeout-sec <n>     Timeout in seconds (default: 30)\n"
        "  --base-page <n>       Page index inside shmdev (default: 0)\n"
        "  --poll-backoff-us <n> Poll backoff in us (default: 10)\n"
        "  --no-reset            Skip zeroing the page before start\n"
        "  --libobmm-path <p>    Path to libobmm.so for CC mode\n"
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
    if (!strcasecmp(s, "cc")) return PROBE_MODE_CC;
    if (!strcasecmp(s, "nc")) return PROBE_MODE_NC;
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
        case 'r': opts->role = parse_role(optarg);          break;
        case 'm': opts->mode = parse_mode(optarg);          break;
        case 'i': opts->memid = parse_u64("memid", optarg); break;
        case 'n': opts->iterations = parse_u64("iters", optarg); break;
        case 'p': opts->payload_bytes = parse_u64("payload-bytes", optarg); break;
        case 't': opts->timeout_sec = parse_u64("timeout-sec", optarg); break;
        case 'b': opts->base_page = parse_u64("base-page", optarg); break;
        case 'B': opts->poll_backoff_us = (int)parse_u64("poll-backoff-us", optarg); break;
        case 'R': opts->reset = false;                      break;
        case 'l': opts->libobmm_path = optarg;              break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default:  usage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    if (opts->memid == 0) {
        fprintf(stderr, "--memid must be non-zero\n");
        exit(EXIT_FAILURE);
    }
    if (opts->iterations == 0) {
        fprintf(stderr, "--iters must be > 0\n");
        exit(EXIT_FAILURE);
    }
    if (opts->payload_bytes < sizeof(uint64_t)) {
        fprintf(stderr, "--payload-bytes must be >= %zu\n", sizeof(uint64_t));
        exit(EXIT_FAILURE);
    }
}

/* --- sysfs helpers --------------------------------------------------- */

static void build_dev_path(char *path, size_t len, uint64_t memid)
{
    int n = snprintf(path, len, OBMM_DEV_PATH_FMT,
                     (unsigned long long)memid);
    if ((n < 0) || ((size_t)n >= len)) {
        fprintf(stderr, "device path too long\n");
        exit(EXIT_FAILURE);
    }
}

static void build_sysfs_path(char *path, size_t len, uint64_t memid,
                             const char *attr)
{
    int n = snprintf(path, len, "%s/obmm_shmdev%llu/%s",
                     OBMM_SYSFS_ROOT, (unsigned long long)memid, attr);
    if ((n < 0) || ((size_t)n >= len)) {
        fprintf(stderr, "sysfs path too long\n");
        exit(EXIT_FAILURE);
    }
}

static uint64_t read_sysfs_hex_u64(uint64_t memid, const char *attr)
{
    char path[OBMM_PATH_MAX], buf[64];
    build_sysfs_path(path, sizeof(path), memid, attr);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fprintf(stderr, "read %s failed\n", path);
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
    uint64_t v;
    if (sscanf(buf, "%" SCNx64, &v) != 1) {
        fprintf(stderr, "parse %s: '%s'\n", path, buf);
        exit(EXIT_FAILURE);
    }
    return v;
}

static long read_sysfs_long(uint64_t memid, const char *attr)
{
    char path[OBMM_PATH_MAX], buf[64];
    build_sysfs_path(path, sizeof(path), memid, attr);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fprintf(stderr, "read %s failed\n", path);
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
    char *end;
    errno = 0;
    long v = strtol(buf, &end, 0);
    if ((errno != 0) || (end == buf)) {
        fprintf(stderr, "parse %s: '%s'\n", path, buf);
        exit(EXIT_FAILURE);
    }
    return v;
}

/* --- libobmm resolution ---------------------------------------------- */

static obmm_set_ownership_func_t
resolve_set_ownership(const char *explicit_path)
{
    static const char *fallbacks[] = {"libobmm.so", "libobmm.so.0"};

    obmm_set_ownership_func_t f;
    f = (obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT, "obmm_set_ownership");
    if (f != NULL) return f;

    if (explicit_path != NULL && *explicit_path != '\0') {
        void *h = dlopen(explicit_path, RTLD_LAZY | RTLD_LOCAL);
        if (h != NULL) {
            f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership");
            if (f != NULL) return f;
        }
    }

    const char *env = getenv("OBMM_LIBOBMM_PATH");
    if (env != NULL && *env != '\0') {
        void *h = dlopen(env, RTLD_LAZY | RTLD_LOCAL);
        if (h != NULL) {
            f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership");
            if (f != NULL) return f;
        }
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

/* --- mapping --------------------------------------------------------- */

static void open_mapping(probe_ctx_t *ctx)
{
    ctx->page_size   = (size_t)sysconf(_SC_PAGESIZE);
    if (ctx->page_size == (size_t)-1) {
        perror("sysconf(_SC_PAGESIZE)");
        exit(EXIT_FAILURE);
    }
    ctx->region_size = read_sysfs_hex_u64(ctx->opts.memid, "size");

    if (read_sysfs_long(ctx->opts.memid, "allow_mmap") == 0) {
        fprintf(stderr, "memid=%" PRIu64 " does not allow mmap\n",
                ctx->opts.memid);
        exit(EXIT_FAILURE);
    }
    if (ctx->opts.payload_bytes + sizeof(uint64_t) > ctx->page_size) {
        fprintf(stderr, "payload_bytes=%" PRIu64 " + seq(8) > page_size=%zu\n",
                ctx->opts.payload_bytes, ctx->page_size);
        exit(EXIT_FAILURE);
    }

    off_t  map_off  = (off_t)(ctx->opts.base_page * ctx->page_size);
    size_t map_len  = ctx->page_size;
    if ((uint64_t)(map_off + (off_t)map_len) > ctx->region_size) {
        fprintf(stderr, "page %" PRIu64 " exceeds region size 0x%" PRIx64 "\n",
                ctx->opts.base_page, ctx->region_size);
        exit(EXIT_FAILURE);
    }

    int open_flags;
    int prot;
    if (ctx->opts.mode == PROBE_MODE_NC) {
        open_flags = O_RDWR | O_SYNC | O_CLOEXEC;
        prot       = PROT_READ | PROT_WRITE;
    } else {
        open_flags = O_RDWR | O_CLOEXEC;
        prot       = PROT_NONE;
        ctx->set_ownership = resolve_set_ownership(ctx->opts.libobmm_path);
    }

    char dev_path[OBMM_PATH_MAX];
    build_dev_path(dev_path, sizeof(dev_path), ctx->opts.memid);
    ctx->fd = open(dev_path, open_flags);
    if (ctx->fd < 0) {
        fprintf(stderr, "open(%s): %s\n", dev_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    ctx->map_base = mmap(NULL, map_len, prot, MAP_SHARED, ctx->fd, map_off);
    if (ctx->map_base == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, len=%zu, off=%lld): %s\n",
                dev_path, map_len, (long long)map_off, strerror(errno));
        close(ctx->fd);
        exit(EXIT_FAILURE);
    }

    ctx->seq_p     = (volatile uint64_t*)ctx->map_base;
    ctx->payload_p = (volatile uint8_t*)(ctx->map_base) + sizeof(uint64_t);
    ctx->poll_backoff_ns = (uint64_t)ctx->opts.poll_backoff_us * 1000ull;
}

static void close_mapping(probe_ctx_t *ctx)
{
    if (ctx->map_base != NULL && ctx->map_base != MAP_FAILED) {
        munmap(ctx->map_base, ctx->page_size);
    }
    if (ctx->fd >= 0) close(ctx->fd);
}

/* --- CC ownership helpers -------------------------------------------- */

/* Flip the entire single-page region to prot, timing the syscall.
 * Dies on failure. */
static void cc_flip(probe_ctx_t *ctx, probe_stats_t *s, int prot)
{
    uint64_t t0 = now_ns();
    int rc = ctx->set_ownership(ctx->fd,
                                ctx->map_base,
                                (char*)ctx->map_base + ctx->page_size,
                                prot);
    uint64_t t1 = now_ns();
    s->ownership_calls++;
    s->ownership_total_ns += (t1 - t0);

    if (rc != 0) {
        fprintf(stderr, "obmm_set_ownership(memid=%" PRIu64 ", prot=%d): %s\n",
                ctx->opts.memid, prot, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* --- payload helpers ------------------------------------------------- */

static void fill_payload(probe_ctx_t *ctx, uint64_t seq)
{
    *(volatile uint64_t*)ctx->payload_p = seq;
    for (uint64_t i = sizeof(uint64_t); i < ctx->opts.payload_bytes; i++) {
        ctx->payload_p[i] = (uint8_t)((seq + i) & 0xffu);
    }
}

static void verify_payload(const probe_ctx_t *ctx, uint64_t expected_seq)
{
    uint64_t seq = *(const volatile uint64_t*)ctx->payload_p;
    if (seq != expected_seq) {
        fprintf(stderr, "seq mismatch: expected=%" PRIu64 " got=%" PRIu64 "\n",
                expected_seq, seq);
        exit(EXIT_FAILURE);
    }
    for (uint64_t i = sizeof(uint64_t); i < ctx->opts.payload_bytes; i++) {
        uint8_t exp = (uint8_t)((expected_seq + i) & 0xffu);
        if (ctx->payload_p[i] != exp) {
            fprintf(stderr, "payload[%" PRIu64 "]: expected=0x%02x got=0x%02x\n",
                    i, exp, ctx->payload_p[i]);
            exit(EXIT_FAILURE);
        }
    }
}

static void poll_backoff(const probe_ctx_t *ctx)
{
    if (ctx->poll_backoff_ns > 0) {
        uint64_t deadline = now_ns() + ctx->poll_backoff_ns;
        while (now_ns() < deadline) {
            probe_cpu_relax();
        }
    }
}

/* --- protocol -------------------------------------------------------- */

/* In CC mode:  acquire PROT_READ, fence, read seq, release to PROT_NONE.
 * In NC mode:  fence, read seq. */
static uint64_t read_seq(probe_ctx_t *ctx, probe_stats_t *s)
{
    uint64_t v;
    if (ctx->opts.mode == PROBE_MODE_CC) {
        cc_flip(ctx, s, PROT_READ);
        probe_bus_full_fence();
        v = *ctx->seq_p;
        cc_flip(ctx, s, PROT_NONE);
    } else {
        probe_bus_full_fence();
        v = *ctx->seq_p;
    }
    return v;
}

/* In CC mode:  acquire PROT_WRITE, fence, write seq + payload, release.
 * In NC mode:  write seq + payload, fence. */
static void write_msg(probe_ctx_t *ctx, probe_stats_t *s, uint64_t seq)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        cc_flip(ctx, s, PROT_WRITE);
        *ctx->seq_p = seq;
        fill_payload(ctx, seq);
        probe_bus_full_fence();
        cc_flip(ctx, s, PROT_NONE);
    } else {
        *ctx->seq_p = seq;
        fill_payload(ctx, seq);
        probe_bus_full_fence();
    }
}

/* In CC mode:  acquire PROT_READ, fence, verify payload,
 *              then acquire PROT_WRITE, write ack (echo seq), release.
 * In NC mode:  fence, verify payload, write ack, fence. */
static void read_and_ack(probe_ctx_t *ctx, probe_stats_t *s, uint64_t seq)
{
    if (ctx->opts.mode == PROBE_MODE_CC) {
        cc_flip(ctx, s, PROT_READ);
        probe_bus_full_fence();
        verify_payload(ctx, seq);
        cc_flip(ctx, s, PROT_WRITE);
        *ctx->seq_p = seq;          /* echo as ack */
        probe_bus_full_fence();
        cc_flip(ctx, s, PROT_NONE);
    } else {
        probe_bus_full_fence();
        verify_payload(ctx, seq);
        *ctx->seq_p = seq;          /* echo as ack */
        probe_bus_full_fence();
    }
}

static void timeout_die(const probe_ctx_t *ctx, const char *phase,
                        uint64_t expected, uint64_t got)
{
    fprintf(stderr,
            "timeout phase=%s mode=%s role=%s expected=%" PRIu64
            " got=%" PRIu64 "\n",
            phase,
            (ctx->opts.mode == PROBE_MODE_CC) ? "cc" : "nc",
            (ctx->opts.role == PROBE_ROLE_A) ? "a" : "b",
            expected, got);
    exit(EXIT_FAILURE);
}

/* Poll `read_seq()` until seq == expected or timeout. */
static void wait_seq(probe_ctx_t *ctx, probe_stats_t *s,
                     uint64_t expected, const char *phase)
{
    uint64_t deadline = now_ns() + (ctx->opts.timeout_sec * 1000000000ull);
    for (;;) {
        uint64_t v = read_seq(ctx, s);
        if (v == expected) return;
        s->poll_loops++;
        if (now_ns() > deadline) {
            timeout_die(ctx, phase, expected, v);
        }
        poll_backoff(ctx);
    }
}

/* --- roles ----------------------------------------------------------- */

static void run_role_a(probe_ctx_t *ctx, probe_stats_t *s)
{
    /* Reset: A zeroes the page so both sides start from seq=0. */
    if (ctx->opts.reset) {
        if (ctx->opts.mode == PROBE_MODE_CC) {
            cc_flip(ctx, s, PROT_WRITE);
            memset(ctx->map_base, 0, ctx->page_size);
            probe_bus_full_fence();
            cc_flip(ctx, s, PROT_NONE);
        } else {
            memset(ctx->map_base, 0, ctx->page_size);
            probe_bus_full_fence();
        }
    }

    for (uint64_t seq = 1; seq <= ctx->opts.iterations; seq++) {
        /* Wait for B to ack the previous message (seq-1). */
        wait_seq(ctx, s, seq - 1, "A_wait_ack_prev");

        uint64_t t0 = now_ns();

        /* Write new message. */
        write_msg(ctx, s, seq);

        /* Wait for B to echo seq (ack). */
        wait_seq(ctx, s, seq, "A_wait_ack");

        uint64_t t1 = now_ns();
        stats_record(s, t1 - t0);
    }
}

static void run_role_b(probe_ctx_t *ctx, probe_stats_t *s)
{
    for (uint64_t seq = 1; seq <= ctx->opts.iterations; seq++) {
        /* Wait for A to write a new message. */
        wait_seq(ctx, s, seq, "B_wait_msg");

        uint64_t t0 = now_ns();

        /* Read payload and echo seq as ack. */
        read_and_ack(ctx, s, seq);

        uint64_t t1 = now_ns();
        stats_record(s, t1 - t0);
    }
}

/* --- main ------------------------------------------------------------ */

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
