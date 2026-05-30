/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 *
 * OBMM CC-vs-NC latency probe — v4.
 *
 * Design: rotating data pages + single-word control page.
 *
 *   Page 0 (control):  volatile uint64_t seq — monotonically increasing
 *                       message counter written by A, read by B.
 *                       B polls until seq >= expected.
 *   Page 1..N (data):   payload pages, used in strict rotation.
 *                       Each page is written ONCE by A, read ONCE by B,
 *                       then left alone until the ring wraps.
 *
 * Protocol (no ack, monotonic doorbell):
 *
 *   A:  choose page P = base+1 + (seq % ring_size)
 *       mmap(P, RW) → write payload → munmap
 *       mmap(ctrl, RW) → write seq → munmap
 *
 *   B:  poll ctrl: mmap(ctrl, R) → read seq → munmap
 *       if seq < expected: backoff, poll again
 *       compute P from seq, read payload, advance
 *
 * Because A never reuses a data page until the ring wraps (and B has
 * consumed it long before), there is zero cross-host ownership conflict
 * on any page.  The control page is the only shared page, used as a
 * one-way doorbell (A writes, B reads, never the reverse).
 *
 * Comparison against v3 cc-mmap:
 *   - v3: poll = mmap(R)+munmap on SAME page → per-poll cost
 *   - v4: poll = mmap(R)+munmap on tiny control page (8 bytes)
 *   - v4 eliminates the ack round-trip entirely
 *
 * Modes (same as v3):
 *   nc            Non-cacheable, persistent RW mapping for control+data
 *   cc-ownership  Cacheable, mmap(PROT_NONE)+obmm_set_ownership on control
 *                 page; data pages accessed via mmap/munmap
 *   cc-mmap       Cacheable, dynamic mmap/munmap for everything
 *
 * Compile:
 *   gcc -O3 -Wall -Wextra -std=gnu11 -o obmm_cc_nc_probe_v4 cc_nc_probe_v4.c -ldl
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
    uint64_t     payload_bytes;    /* per-msg payload (must fit in page)   */
    uint64_t     timeout_sec;
    uint64_t     base_page;        /* first page index in shmdev           */
    unsigned     ring_size;        /* number of data pages (default 64)    */
    int          poll_backoff_us;
    bool         reset;
    const char  *libobmm_path;
} probe_opts_t;

typedef struct {
    uint64_t *samples_ns;
    uint64_t  total_ns;
    uint64_t  min_ns;
    uint64_t  max_ns;
    uint64_t  access_syscalls;     /* mmap/munmap/set_ownership calls      */
    uint64_t  access_syscall_ns;
    uint64_t  poll_loops;
    uint64_t  count;
} probe_stats_t;

typedef struct {
    probe_opts_t               opts;
    int                        fd;
    size_t                     page_size;
    char                       dev_path[OBMM_PATH_MAX];
    uint64_t                   region_size;

    /* persistent mappings (nc / cc-ownership) */
    void                      *fixed_ctrl;       /* control page            */
    void                      *fixed_data_base;  /* first data page         */
    obmm_set_ownership_func_t  set_ownership;    /* cc-ownership only       */

    /* working pointer — valid only inside an access window */
    void                      *cur_map;
    uint64_t                   poll_backoff_ns;
} probe_ctx_t;

/* ====================================================================
 * helpers
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
    uint64_t va = *(const uint64_t*)a, vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

static uint64_t percentile_u64(const uint64_t *s, uint64_t n, uint64_t num, uint64_t den)
{
    if (n == 0) return 0;
    return s[((n - 1) * num) / den];
}

static void stats_init(probe_stats_t *st, uint64_t count)
{
    memset(st, 0, sizeof(*st));
    st->samples_ns = calloc((size_t)count, sizeof(*st->samples_ns));
    if (st->samples_ns == NULL) { fprintf(stderr, "OOM\n"); exit(EXIT_FAILURE); }
    st->min_ns = UINT64_MAX;
}

static void stats_record(probe_stats_t *st, uint64_t ns)
{
    st->samples_ns[st->count++] = ns;
    st->total_ns += ns;
    if (ns < st->min_ns) st->min_ns = ns;
    if (ns > st->max_ns) st->max_ns = ns;
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
           " ring_size=%u"
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
           ctx->opts.ring_size,
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
        "Modes: nc | cc-ownership | cc-mmap\n"
        "Options:\n"
        "  --iters <n>           Messages (default: 100)\n"
        "  --payload-bytes <n>   Payload per message (default: 8)\n"
        "  --timeout-sec <n>     Timeout (default: 30)\n"
        "  --base-page <n>       First page index (default: 0)\n"
        "  --ring-size <n>       Data pages in ring (default: 64)\n"
        "  --poll-backoff-us <n> Poll backoff (default: 10)\n"
        "  --no-reset            Skip init zeroing\n"
        "  --libobmm-path <p>    Path to libobmm.so (cc-ownership)\n"
        "  -h, --help\n",
        prog);
}

static uint64_t parse_u64(const char *name, const char *value)
{
    char *end; errno = 0;
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
    fprintf(stderr, "invalid role: '%s'\n", s); exit(EXIT_FAILURE);
}

static probe_mode_t parse_mode(const char *s)
{
    if (!strcasecmp(s, "nc"))            return PROBE_MODE_NC;
    if (!strcasecmp(s, "cc-ownership"))  return PROBE_MODE_CC_OWNERSHIP;
    if (!strcasecmp(s, "cc-mmap"))       return PROBE_MODE_CC_MMAP;
    fprintf(stderr, "invalid mode: '%s'\n", s); exit(EXIT_FAILURE);
}

static void parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    static const struct option lo[] = {
        {"role",           required_argument, NULL, 'r'},
        {"mode",           required_argument, NULL, 'm'},
        {"memid",          required_argument, NULL, 'i'},
        {"iters",          required_argument, NULL, 'n'},
        {"payload-bytes",  required_argument, NULL, 'p'},
        {"timeout-sec",    required_argument, NULL, 't'},
        {"base-page",      required_argument, NULL, 'b'},
        {"ring-size",      required_argument, NULL, 'R'},
        {"poll-backoff-us",required_argument, NULL, 'B'},
        {"no-reset",       no_argument,       NULL, 'N'},
        {"libobmm-path",   required_argument, NULL, 'l'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    memset(opts, 0, sizeof(*opts));
    opts->iterations      = 100;
    opts->payload_bytes   = sizeof(uint64_t);
    opts->timeout_sec     = 30;
    opts->ring_size       = 64;
    opts->poll_backoff_us = 10;
    opts->reset           = true;

    while ((opt = getopt_long(argc, argv, "r:m:i:n:p:t:b:R:B:Nl:h",
                              lo, NULL)) != -1) {
        switch (opt) {
        case 'r': opts->role = parse_role(optarg);  break;
        case 'm': opts->mode = parse_mode(optarg);  break;
        case 'i': opts->memid = parse_u64("memid", optarg); break;
        case 'n': opts->iterations = parse_u64("iters", optarg); break;
        case 'p': opts->payload_bytes = parse_u64("payload-bytes", optarg); break;
        case 't': opts->timeout_sec = parse_u64("timeout-sec", optarg); break;
        case 'b': opts->base_page = parse_u64("base-page", optarg); break;
        case 'R': opts->ring_size = (unsigned)parse_u64("ring-size", optarg); break;
        case 'B': opts->poll_backoff_us = (int)parse_u64("poll-backoff-us", optarg); break;
        case 'N': opts->reset = false; break;
        case 'l': opts->libobmm_path = optarg; break;
        case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
        default:  usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (opts->memid == 0)      { fprintf(stderr, "--memid required\n"); exit(1); }
    if (opts->iterations == 0) { fprintf(stderr, "--iters > 0\n"); exit(1); }
    if (opts->ring_size < 2)   { fprintf(stderr, "--ring-size >= 2\n"); exit(1); }
    if (opts->payload_bytes < sizeof(uint64_t)) {
        fprintf(stderr, "--payload-bytes >= %zu\n", sizeof(uint64_t)); exit(1);
    }
}

/* ====================================================================
 * sysfs
 * ==================================================================== */

static uint64_t read_sysfs_hex_u64(uint64_t memid, const char *attr)
{
    char path[OBMM_PATH_MAX], buf[64];
    snprintf(path, sizeof(path), "%s/obmm_shmdev%llu/%s",
             OBMM_SYSFS_ROOT, (unsigned long long)memid, attr);
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(1); }
    fgets(buf, sizeof(buf), fp); fclose(fp);
    uint64_t v;
    if (sscanf(buf, "%" SCNx64, &v) != 1) { fprintf(stderr, "parse %s\n", path); exit(1); }
    return v;
}

static long read_sysfs_long(uint64_t memid, const char *attr)
{
    char path[OBMM_PATH_MAX], buf[64];
    snprintf(path, sizeof(path), "%s/obmm_shmdev%llu/%s",
             OBMM_SYSFS_ROOT, (unsigned long long)memid, attr);
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(1); }
    fgets(buf, sizeof(buf), fp); fclose(fp);
    char *end; errno = 0;
    long v = strtol(buf, &end, 0);
    if ((errno != 0) || (end == buf)) { fprintf(stderr, "parse %s\n", path); exit(1); }
    return v;
}

/* ====================================================================
 * libobmm
 * ==================================================================== */

static obmm_set_ownership_func_t
resolve_set_ownership(const char *explicit_path)
{
    static const char *fb[] = {"libobmm.so", "libobmm.so.0"};
    obmm_set_ownership_func_t f;
    f = (obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT, "obmm_set_ownership");
    if (f) return f;
    if (explicit_path && *explicit_path) {
        void *h = dlopen(explicit_path, RTLD_LAZY|RTLD_LOCAL);
        if (h) { f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership"); if (f) return f; }
    }
    const char *env = getenv("OBMM_LIBOBMM_PATH");
    if (env && *env) {
        void *h = dlopen(env, RTLD_LAZY|RTLD_LOCAL);
        if (h) { f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership"); if (f) return f; }
    }
    for (size_t i = 0; i < sizeof(fb)/sizeof(fb[0]); i++) {
        void *h = dlopen(fb[i], RTLD_LAZY|RTLD_LOCAL);
        if (!h) continue;
        f = (obmm_set_ownership_func_t)dlsym(h, "obmm_set_ownership");
        if (f) return f;
    }
    fprintf(stderr, "cannot resolve obmm_set_ownership\n"); exit(1);
}

/* ====================================================================
 * page offset helpers
 *
 * Layout:  page 0 = control,  pages 1..ring_size = data ring.
 * Total pages needed: 1 + ring_size.
 * ==================================================================== */

static off_t ctrl_off(const probe_ctx_t *ctx)
{ return (off_t)(ctx->opts.base_page * ctx->page_size); }

static off_t data_off(const probe_ctx_t *ctx, uint64_t seq)
{
    unsigned idx = (unsigned)((seq - 1) % ctx->opts.ring_size);
    return (off_t)((ctx->opts.base_page + 1 + idx) * ctx->page_size);
}

/* ====================================================================
 * setup / teardown
 * ==================================================================== */

static void setup(probe_ctx_t *ctx)
{
    ctx->page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (ctx->page_size == (size_t)-1) { perror("sysconf"); exit(1); }
    ctx->region_size = read_sysfs_hex_u64(ctx->opts.memid, "size");

    if (read_sysfs_long(ctx->opts.memid, "allow_mmap") == 0) {
        fprintf(stderr, "memid=%" PRIu64 " does not allow mmap\n", ctx->opts.memid); exit(1);
    }
    if (ctx->opts.payload_bytes > ctx->page_size) {
        fprintf(stderr, "payload > page_size\n"); exit(1);
    }

    /* check that the full range [base_page, base_page+1+ring_size) fits */
    uint64_t total_pages = 1 + ctx->opts.ring_size;
    uint64_t end_off     = (ctx->opts.base_page + total_pages) * ctx->page_size;
    if (end_off > ctx->region_size) {
        fprintf(stderr, "need %" PRIu64 " pages (ctrl+%u data), region too small\n",
                total_pages, ctx->opts.ring_size);
        exit(1);
    }

    snprintf(ctx->dev_path, sizeof(ctx->dev_path), OBMM_DEV_PATH_FMT,
             (unsigned long long)ctx->opts.memid);

    int fl = (ctx->opts.mode == PROBE_MODE_NC) ? (O_RDWR | O_SYNC | O_CLOEXEC)
                                                : (O_RDWR | O_CLOEXEC);
    ctx->fd = open(ctx->dev_path, fl);
    if (ctx->fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); exit(1); }

    /* persistent mappings for nc / cc-ownership modes */
    if (ctx->opts.mode == PROBE_MODE_NC) {
        /* map entire control+data range once, RW */
        size_t total_len = total_pages * ctx->page_size;
        off_t  total_off = ctrl_off(ctx);
        void *p = mmap(NULL, total_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fd, total_off);
        if (p == MAP_FAILED) { fprintf(stderr, "mmap nc: %s\n", strerror(errno)); exit(1); }
        ctx->fixed_ctrl      = p;
        ctx->fixed_data_base = (char*)p + ctx->page_size;
    } else if (ctx->opts.mode == PROBE_MODE_CC_OWNERSHIP) {
        ctx->set_ownership = resolve_set_ownership(ctx->opts.libobmm_path);
        /* map ctrl page PROT_NONE, data pages not mapped */
        void *p = mmap(NULL, ctx->page_size, PROT_NONE,
                       MAP_SHARED, ctx->fd, ctrl_off(ctx));
        if (p == MAP_FAILED) { fprintf(stderr, "mmap cc-own: %s\n", strerror(errno)); exit(1); }
        ctx->fixed_ctrl      = p;
        ctx->fixed_data_base = NULL;
    }
    /* cc-mmap: nothing mapped */

    ctx->poll_backoff_ns = (uint64_t)ctx->opts.poll_backoff_us * 1000ull;
}

static void teardown(probe_ctx_t *ctx)
{
    if (ctx->opts.mode == PROBE_MODE_NC) {
        size_t total = (1 + ctx->opts.ring_size) * ctx->page_size;
        if (ctx->fixed_ctrl) munmap(ctx->fixed_ctrl, total);
    } else if (ctx->opts.mode == PROBE_MODE_CC_OWNERSHIP) {
        if (ctx->fixed_ctrl) munmap(ctx->fixed_ctrl, ctx->page_size);
    }
    if (ctx->fd >= 0) close(ctx->fd);
}

/* ====================================================================
 * access discipline
 * ==================================================================== */

static void syscall_time(probe_stats_t *s, uint64_t t0, uint64_t t1)
{
    s->access_syscalls++;
    s->access_syscall_ns += (t1 - t0);
}

/* --- mmap / munmap (cc-mmap mode) ------------------------------------ */

static void *do_mmap(probe_ctx_t *ctx, probe_stats_t *s,
                     off_t off, int prot)
{
    uint64_t t0 = now_ns();
    void *p = mmap(NULL, ctx->page_size, prot, MAP_SHARED, ctx->fd, off);
    uint64_t t1 = now_ns();
    syscall_time(s, t0, t1);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap(off=0x%lx,prot=%d): %s\n",
                (unsigned long)off, prot, strerror(errno));
        exit(1);
    }
    return p;
}

static void do_munmap(probe_ctx_t *ctx, probe_stats_t *s, void *p)
{
    (void)ctx;
    uint64_t t0 = now_ns();
    int rc = munmap(p, ctx->page_size);
    uint64_t t1 = now_ns();
    syscall_time(s, t0, t1);
    if (rc != 0) { fprintf(stderr, "munmap: %s\n", strerror(errno)); exit(1); }
}

/* --- set_ownership (cc-ownership mode, ctrl page only) --------------- */

static void do_flip(probe_ctx_t *ctx, probe_stats_t *s, int prot)
{
    uint64_t t0 = now_ns();
    int rc = ctx->set_ownership(ctx->fd, ctx->fixed_ctrl,
                                (char*)ctx->fixed_ctrl + ctx->page_size, prot);
    uint64_t t1 = now_ns();
    syscall_time(s, t0, t1);
    if (rc != 0) {
        fprintf(stderr, "set_ownership(prot=%d): %s\n", prot, strerror(errno));
        exit(1);
    }
}

/* --- read control word ----------------------------------------------- */

static uint64_t read_ctrl(probe_ctx_t *ctx, probe_stats_t *s)
{
    uint64_t v;
    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        probe_bus_full_fence();
        v = *(volatile uint64_t*)ctx->fixed_ctrl;
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        do_flip(ctx, s, PROT_READ);
        probe_bus_full_fence();
        v = *(volatile uint64_t*)ctx->fixed_ctrl;
        do_flip(ctx, s, PROT_NONE);
        break;
    case PROBE_MODE_CC_MMAP:
        ctx->cur_map = do_mmap(ctx, s, ctrl_off(ctx), PROT_READ);
        probe_bus_full_fence();
        v = *(volatile uint64_t*)ctx->cur_map;
        do_munmap(ctx, s, ctx->cur_map);
        break;
    }
    return v;
}

/* --- write control word ---------------------------------------------- */

static void write_ctrl(probe_ctx_t *ctx, probe_stats_t *s, uint64_t val)
{
    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        *(volatile uint64_t*)ctx->fixed_ctrl = val;
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        do_flip(ctx, s, PROT_WRITE);
        *(volatile uint64_t*)ctx->fixed_ctrl = val;
        probe_bus_full_fence();
        do_flip(ctx, s, PROT_NONE);
        break;
    case PROBE_MODE_CC_MMAP:
        ctx->cur_map = do_mmap(ctx, s, ctrl_off(ctx), PROT_READ | PROT_WRITE);
        *(volatile uint64_t*)ctx->cur_map = val;
        probe_bus_full_fence();
        do_munmap(ctx, s, ctx->cur_map);
        break;
    }
}

/* --- write data page ------------------------------------------------- */

static void write_data(probe_ctx_t *ctx, probe_stats_t *s,
                       uint64_t seq, uint8_t byte)
{
    off_t off = data_off(ctx, seq);
    void *p;

    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        p = (char*)ctx->fixed_data_base
          + (size_t)((seq - 1) % ctx->opts.ring_size) * ctx->page_size;
        memset(p, (int)byte, ctx->opts.payload_bytes);
        *(volatile uint64_t*)p = seq;   /* stamp seq at offset 0 */
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        /* data pages: always mmap/munmap (no set_ownership on data path) */
        p = do_mmap(ctx, s, off, PROT_READ | PROT_WRITE);
        memset(p, (int)byte, ctx->opts.payload_bytes);
        *(volatile uint64_t*)p = seq;
        probe_bus_full_fence();
        do_munmap(ctx, s, p);
        break;
    case PROBE_MODE_CC_MMAP:
        p = do_mmap(ctx, s, off, PROT_READ | PROT_WRITE);
        memset(p, (int)byte, ctx->opts.payload_bytes);
        *(volatile uint64_t*)p = seq;
        probe_bus_full_fence();
        do_munmap(ctx, s, p);
        break;
    }
}

/* --- read data page -------------------------------------------------- */

static void read_data(probe_ctx_t *ctx, probe_stats_t *s,
                      uint64_t seq, uint64_t expected_seq, uint8_t expected_byte)
{
    off_t off = data_off(ctx, seq);
    void *p;

    switch (ctx->opts.mode) {
    case PROBE_MODE_NC:
        p = (char*)ctx->fixed_data_base
          + (size_t)((seq - 1) % ctx->opts.ring_size) * ctx->page_size;
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_OWNERSHIP:
        p = do_mmap(ctx, s, off, PROT_READ);
        probe_bus_full_fence();
        break;
    case PROBE_MODE_CC_MMAP:
        p = do_mmap(ctx, s, off, PROT_READ);
        probe_bus_full_fence();
        break;
    }

    uint64_t stamp = *(volatile uint64_t*)p;
    if (stamp != expected_seq) {
        fprintf(stderr, "data seq mismatch: expected=%" PRIu64 " got=%" PRIu64 "\n",
                expected_seq, stamp);
        exit(1);
    }
    /* spot-check a few payload bytes */
    uint8_t *pl = (uint8_t*)p + sizeof(uint64_t);
    for (uint64_t i = 0; i < ctx->opts.payload_bytes - sizeof(uint64_t); i += 997) {
        if (pl[i] != expected_byte) {
            fprintf(stderr, "payload[%" PRIu64 "]=0x%02x expected 0x%02x\n",
                    i, pl[i], expected_byte);
            exit(1);
        }
    }

    if (ctx->opts.mode != PROBE_MODE_NC) {
        do_munmap(ctx, s, p);
    }
}

/* ====================================================================
 * protocol — no ack, one-way doorbell
 *
 * A:  write_data(seq) → write_ctrl(data_offset)
 * B:  poll ctrl until head != last_seen → read_data(seq)
 *
 * A measures from start-of-write-data to ctrl-published.
 * B measures from ctrl-seen to end-of-read-data.
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
    exit(1);
}

static void run_role_a(probe_ctx_t *ctx, probe_stats_t *s)
{
    /* Reset: zero the control page so B starts from seq=0. */
    write_ctrl(ctx, s, 0);

    for (uint64_t seq = 1; seq <= ctx->opts.iterations; seq++) {
        uint8_t byte = (uint8_t)(seq & 0xffu);

        uint64_t t0 = now_ns();

        write_data(ctx, s, seq, byte);
        write_ctrl(ctx, s, seq);   /* doorbell: monotonically increasing seq */

        uint64_t t1 = now_ns();
        stats_record(s, t1 - t0);
    }
}

static void run_role_b(probe_ctx_t *ctx, probe_stats_t *s)
{
    uint64_t dl = now_ns() + (ctx->opts.timeout_sec * 1000000000ull);

    for (uint64_t seq = 1; seq <= ctx->opts.iterations; seq++) {
        /* poll until ctrl >= seq (monotonic counter, tolerates A lapping) */
        uint64_t ctrl;
        for (;;) {
            ctrl = read_ctrl(ctx, s);
            if (ctrl >= seq) break;
            s->poll_loops++;
            if (now_ns() > dl) timeout_die(ctx, "B_wait_ctrl", seq, ctrl);
            poll_backoff(ctx);
        }

        uint64_t t0 = now_ns();

        uint8_t expected_byte = (uint8_t)(seq & 0xffu);
        read_data(ctx, s, seq, seq, expected_byte);

        uint64_t t1 = now_ns();
        stats_record(s, t1 - t0);
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
