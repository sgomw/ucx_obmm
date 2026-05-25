/*
 * obmm_bulk_window_probe.c
 *
 * Cross-node bulk-window bandwidth probe that compares:
 *   1. NC shared-memory windows over OBMM shmdev (O_SYNC)
 *   2. CC shared-memory windows over OBMM shmdev with ownership handoff
 *
 * The control plane always uses one small NC page, so NC and CC runs share the
 * same request/ack protocol. The measurement is therefore focused on the bulk
 * data path:
 *   - NC mode: sender writes a window in NC export memory, receiver reads the
 *              corresponding window from the NC import mapping
 *   - CC mode: sender writes a cacheable export window, releases it to READ,
 *              receiver acquires READ on the corresponding import window,
 *              copies it out, releases the import window to NONE, and sender
 *              reacquires WRITE before reusing that window
 *
 * Unlike the per-message latency probe, this program pipelines multiple data
 * windows. Each window carries one large bulk epoch, so ownership cost is
 * amortized over `window_size` bytes instead of one tiny message.
 *
 * Build:
 *   gcc -O3 -std=gnu11 -Wall -Wextra -o obmm_bulk_window_probe \
 *       obmm_bulk_window_probe.c -ldl -lrt
 *
 * Example (run the same command on both nodes, changing only --role):
 *   ./obmm_bulk_window_probe --role server --mode both --run-id 21 \
 *       --window-size 2M --window-count 4 --iters 128 --warmup 8
 *
 *   ./obmm_bulk_window_probe --role client --mode both --run-id 21 \
 *       --window-size 2M --window-count 4 --iters 128 --warmup 8
 *
 * Defaults match the current test setup:
 *   NC export/import memids: 1 / 2
 *   CC export/import memids: 3 / 4
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef int (*probe_obmm_set_ownership_func_t)(int fd, void *start, void *end,
                                               int prot);

#define PROBE_CTRL_MAGIC             0x504d4d50u
#define PROBE_DEFAULT_ITERS          128u
#define PROBE_DEFAULT_WARMUP         8u
#define PROBE_DEFAULT_WINDOW_SIZE    (2ul * 1024ul * 1024ul)
#define PROBE_DEFAULT_WINDOW_COUNT   4u
#define PROBE_DEFAULT_PMD_SIZE       (2ul * 1024ul * 1024ul)
#define PROBE_DEFAULT_NC_EXPORT      1ull
#define PROBE_DEFAULT_NC_IMPORT      2ull
#define PROBE_DEFAULT_CC_EXPORT      3ull
#define PROBE_DEFAULT_CC_IMPORT      4ull
#define PROBE_DEFAULT_NC_OFFSET      0ul
#define PROBE_DEFAULT_CC_OFFSET      0ul
#define PROBE_CACHELINE              64u

typedef enum {
    PROBE_ROLE_SERVER = 0,
    PROBE_ROLE_CLIENT = 1
} probe_role_t;

typedef enum {
    PROBE_MODE_NC   = 0,
    PROBE_MODE_CC   = 1,
    PROBE_MODE_BOTH = 2
} probe_mode_t;

typedef struct __attribute__((aligned(PROBE_CACHELINE))) probe_stats {
    uint64_t wait_credit_ns;
    uint64_t wait_drain_ns;
    uint64_t wait_req_ns;
    uint64_t tx_copy_ns;
    uint64_t rx_copy_ns;
    uint64_t ctrl_publish_ns;
    uint64_t tx_release_ns;
    uint64_t tx_reacquire_ns;
    uint64_t rx_acquire_ns;
    uint64_t rx_release_ns;
    uint64_t credit_spins;
    uint64_t drain_spins;
    uint64_t req_spins;
    uint64_t checksum;
} probe_stats_t;

typedef struct __attribute__((aligned(PROBE_CACHELINE))) probe_ctrl {
    volatile uint32_t magic;
    volatile uint32_t run_id;
    volatile uint32_t phase_id;
    volatile uint32_t ready;
    volatile uint32_t done;
    volatile uint32_t mode;
    volatile uint32_t role;
    volatile uint32_t error;
    volatile uint64_t window_size;
    volatile uint64_t window_count;
    volatile uint64_t iters;
    volatile uint64_t warmup;
    volatile uint64_t req_seq;
    volatile uint64_t ack_seq;
    probe_stats_t     stats;
} probe_ctrl_t;

typedef struct probe_opts {
    probe_role_t role;
    probe_mode_t mode;
    int          role_set;
    uint32_t     run_id;
    unsigned     iters;
    unsigned     warmup;
    unsigned     window_count;
    size_t       window_size;
    int          cpu;
    uint64_t     nc_export_memid;
    uint64_t     nc_import_memid;
    uint64_t     cc_export_memid;
    uint64_t     cc_import_memid;
    off_t        nc_offset;
    off_t        cc_offset;
} probe_opts_t;

typedef struct probe_mapping {
    void  *base;
    size_t length;
    int    fd;
    off_t  offset;
    char   dev_path[128];
} probe_mapping_t;

typedef struct probe_context {
    const probe_opts_t *opts;
    size_t              page_size;
    size_t              pmd_size;
    size_t              data_bytes;
    size_t              nc_map_len;
    size_t              cc_map_len;
    probe_mapping_t     nc_local;
    probe_mapping_t     nc_peer;
    probe_mapping_t     cc_local_export;
    probe_mapping_t     cc_peer_import;
} probe_context_t;

typedef struct probe_phase_ctx {
    probe_mode_t  mode;
    const char   *name;
    uint32_t      phase_id;
    probe_ctrl_t *local_ctrl;
    probe_ctrl_t *peer_ctrl;
    uint8_t      *data_base;
    int           data_fd;
} probe_phase_ctx_t;

typedef struct probe_phase_result {
    const char *name;
    double      gbps;
} probe_phase_result_t;

static void probe_usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Required options:\n"
            "  --role <server|client>\n"
            "  --run-id <u32>\n"
            "\n"
            "Other options:\n"
            "  --mode <nc|cc|both>        Benchmark mode (default: both)\n"
            "  --window-size <bytes>      Bytes per bulk window (default: 2M)\n"
            "  --window-count <count>     In-flight bulk windows (default: 4)\n"
            "  --iters <count>            Measured windows (default: %u)\n"
            "  --warmup <count>           Warmup windows (default: %u)\n"
            "  --cpu <cpu>                Pin process to one CPU\n"
            "  --nc-export-memid <id>     NC export memid (default: 1)\n"
            "  --nc-import-memid <id>     NC import memid (default: 2)\n"
            "  --cc-export-memid <id>     CC export memid (default: 3)\n"
            "  --cc-import-memid <id>     CC import memid (default: 4)\n"
            "  --nc-offset <bytes>        NC control-page offset (default: 0)\n"
            "  --cc-offset <bytes>        CC data offset (default: 0)\n"
            "\n"
            "Notes:\n"
            "  - The NC region always provides the request/ack control page.\n"
            "  - NC data windows begin one page after --nc-offset.\n"
            "  - CC uses independent windows of --window-size bytes starting at\n"
            "    --cc-offset, one ownership epoch per window.\n"
            "  - For CC, both --cc-offset and --window-size must be 2M-aligned.\n"
            "  - Change --run-id between independent runs to avoid stale control\n"
            "    state from a prior execution.\n",
            progname, PROBE_DEFAULT_ITERS, PROBE_DEFAULT_WARMUP);
}

static uint64_t probe_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + ts.tv_nsec;
}

static int probe_parse_size(const char *str, size_t *value_p)
{
    char     *end;
    uint64_t  value;
    uint64_t  mult = 1;

    if ((str == NULL) || (*str == '\0')) {
        return -1;
    }

    errno = 0;
    value = strtoull(str, &end, 0);
    if ((errno != 0) || (end == str)) {
        return -1;
    }

    if (*end != '\0') {
        if ((end[1] != '\0') && !((end[1] == 'i') && (end[2] == '\0'))) {
            return -1;
        }

        switch (*end) {
        case 'k':
        case 'K':
            mult = 1024ull;
            break;
        case 'm':
        case 'M':
            mult = 1024ull * 1024ull;
            break;
        case 'g':
        case 'G':
            mult = 1024ull * 1024ull * 1024ull;
            break;
        default:
            return -1;
        }
    }

    if (value > (SIZE_MAX / mult)) {
        return -1;
    }

    *value_p = (size_t)(value * mult);
    return 0;
}

static int probe_parse_u64(const char *str, uint64_t *value_p)
{
    char     *end;
    uint64_t  value;

    errno = 0;
    value = strtoull(str, &end, 0);
    if ((errno != 0) || (end == str) || (*end != '\0')) {
        return -1;
    }

    *value_p = value;
    return 0;
}

static int probe_parse_u32(const char *str, uint32_t *value_p)
{
    uint64_t value;

    if ((probe_parse_u64(str, &value) != 0) || (value > UINT32_MAX)) {
        return -1;
    }

    *value_p = (uint32_t)value;
    return 0;
}

static int probe_parse_role(const char *role_str, probe_role_t *role_p)
{
    if (!strcmp(role_str, "server")) {
        *role_p = PROBE_ROLE_SERVER;
    } else if (!strcmp(role_str, "client")) {
        *role_p = PROBE_ROLE_CLIENT;
    } else {
        return -1;
    }

    return 0;
}

static int probe_parse_mode(const char *mode_str, probe_mode_t *mode_p)
{
    if (!strcmp(mode_str, "nc")) {
        *mode_p = PROBE_MODE_NC;
    } else if (!strcmp(mode_str, "cc")) {
        *mode_p = PROBE_MODE_CC;
    } else if (!strcmp(mode_str, "both")) {
        *mode_p = PROBE_MODE_BOTH;
    } else {
        return -1;
    }

    return 0;
}

static const char *probe_mode_name(probe_mode_t mode)
{
    return (mode == PROBE_MODE_CC) ? "cc" : "nc";
}

static int probe_set_affinity(int cpu)
{
    cpu_set_t cpuset;

    if (cpu < 0) {
        return 0;
    }

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

static inline void probe_cacheable_store_fence(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void probe_cacheable_load_fence(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static inline void probe_bus_store_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("sfence" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

static inline void probe_bus_load_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("lfence" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

static inline void probe_spin_hint(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

static inline uint32_t probe_u32_load(const volatile uint32_t *ptr)
{
    return *ptr;
}

static inline void probe_u32_store(volatile uint32_t *ptr, uint32_t value)
{
    *ptr = value;
}

static inline uint64_t probe_u64_load(const volatile uint64_t *ptr)
{
    return *ptr;
}

static inline void probe_u64_store(volatile uint64_t *ptr, uint64_t value)
{
    *ptr = value;
}

static probe_obmm_set_ownership_func_t probe_resolve_obmm_set_ownership(void)
{
    static probe_obmm_set_ownership_func_t func;
    static int                             resolved;
    static void                           *handles[3];
    const char                            *env_path;
    const char                            *candidates[2];
    int                                    i;

    if (resolved) {
        return func;
    }

    resolved = 1;
    func = (probe_obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT,
                                                  "obmm_set_ownership");
    if (func != NULL) {
        return func;
    }

    env_path      = getenv("OBMM_LIBOBMM_PATH");
    candidates[0] = "libobmm.so";
    candidates[1] = "libobmm.so.0";

    if ((env_path != NULL) && (*env_path != '\0')) {
        handles[0] = dlopen(env_path, RTLD_LAZY | RTLD_LOCAL);
        if (handles[0] != NULL) {
            func = (probe_obmm_set_ownership_func_t)dlsym(handles[0],
                                                          "obmm_set_ownership");
            if (func != NULL) {
                return func;
            }
        }
    }

    for (i = 0; i < 2; ++i) {
        handles[i + 1] = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL);
        if (handles[i + 1] == NULL) {
            continue;
        }

        func = (probe_obmm_set_ownership_func_t)dlsym(handles[i + 1],
                                                      "obmm_set_ownership");
        if (func != NULL) {
            return func;
        }
    }

    return NULL;
}

static uint64_t probe_checksum_bytes(const uint8_t *buf, size_t size)
{
    uint64_t sum = 0;

    if (size == 0) {
        return 0;
    }

    sum += buf[0];
    sum += buf[size - 1];
    if (size > 2) {
        sum += buf[size / 2];
    }
    return sum;
}

static void probe_signal_error(probe_ctrl_t *local_ctrl, int err)
{
    probe_u32_store(&local_ctrl->error, (uint32_t)((err != 0) ? err : EIO));
    probe_bus_store_fence();
    probe_u32_store(&local_ctrl->done, 1);
}

static int probe_build_dev_path(uint64_t memid, char *dev_path, size_t dev_path_len)
{
    int ret;

    ret = snprintf(dev_path, dev_path_len, "/dev/obmm_shmdev%" PRIu64, memid);
    return ((ret < 0) || ((size_t)ret >= dev_path_len)) ? -1 : 0;
}

static int probe_open_obmm_mapping(probe_mapping_t *mapping, uint64_t memid,
                                   int open_flags, int prot, off_t offset,
                                   size_t map_len)
{
    int fd;

    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
    if (probe_build_dev_path(memid, mapping->dev_path,
                             sizeof(mapping->dev_path)) != 0) {
        fprintf(stderr, "failed to build obmm_shmdev path for memid=%" PRIu64 "\n",
                memid);
        return -1;
    }

    fd = open(mapping->dev_path, open_flags);
    if (fd < 0) {
        perror(mapping->dev_path);
        return -1;
    }

    mapping->base = mmap(NULL, map_len, prot, MAP_SHARED, fd, offset);
    if (mapping->base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    mapping->fd     = fd;
    mapping->length = map_len;
    mapping->offset = offset;
    return 0;
}

static void probe_close_mapping(probe_mapping_t *mapping)
{
    if (mapping->base && (mapping->base != MAP_FAILED)) {
        munmap(mapping->base, mapping->length);
    }
    if (mapping->fd >= 0) {
        close(mapping->fd);
    }
    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
}

static int probe_set_ownership_timed(int fd, void *start, size_t length, int prot,
                                     uint64_t *bucket)
{
    probe_obmm_set_ownership_func_t set_ownership;
    uint64_t                        start_ns;

    set_ownership = probe_resolve_obmm_set_ownership();
    if (set_ownership == NULL) {
        fprintf(stderr,
                "failed to resolve obmm_set_ownership; "
                "set OBMM_LIBOBMM_PATH or put libobmm.so in the loader path\n");
        errno = ENOSYS;
        return -1;
    }

    start_ns = 0;
    if (bucket != NULL) {
        start_ns = probe_now_ns();
    }

    if (set_ownership(fd, start, (void*)((uintptr_t)start + length), prot) != 0) {
        fprintf(stderr,
                "obmm_set_ownership failed fd=%d start=%p length=%zu prot=%d: %s\n",
                fd, start, length, prot, strerror(errno));
        return -1;
    }

    if (bucket != NULL) {
        *bucket += probe_now_ns() - start_ns;
    }

    return 0;
}

static int probe_open_context(const probe_opts_t *opts, probe_context_t *ctx)
{
    long page_size;

    memset(ctx, 0, sizeof(*ctx));
    ctx->opts      = opts;
    ctx->pmd_size  = PROBE_DEFAULT_PMD_SIZE;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return -1;
    }

    ctx->page_size = (size_t)page_size;
    if (opts->window_count > (SIZE_MAX / opts->window_size)) {
        fprintf(stderr, "window_count * window_size overflows size_t\n");
        return -1;
    }

    ctx->data_bytes = opts->window_size * opts->window_count;
    ctx->nc_map_len = ctx->page_size +
                      (((opts->mode == PROBE_MODE_NC) ||
                        (opts->mode == PROBE_MODE_BOTH)) ? ctx->data_bytes : 0);

    if (probe_open_obmm_mapping(&ctx->nc_local, opts->nc_export_memid,
                                O_RDWR | O_SYNC, PROT_READ | PROT_WRITE,
                                opts->nc_offset, ctx->nc_map_len) != 0) {
        return -1;
    }

    if (probe_open_obmm_mapping(&ctx->nc_peer, opts->nc_import_memid,
                                O_RDWR | O_SYNC, PROT_READ | PROT_WRITE,
                                opts->nc_offset, ctx->nc_map_len) != 0) {
        probe_close_mapping(&ctx->nc_local);
        return -1;
    }

    if ((opts->mode == PROBE_MODE_CC) || (opts->mode == PROBE_MODE_BOTH)) {
        ctx->cc_map_len = ctx->data_bytes;

        if (opts->role == PROBE_ROLE_CLIENT) {
            if (probe_open_obmm_mapping(&ctx->cc_local_export, opts->cc_export_memid,
                                        O_RDWR, PROT_READ | PROT_WRITE,
                                        opts->cc_offset, ctx->cc_map_len) != 0) {
                probe_close_mapping(&ctx->nc_peer);
                probe_close_mapping(&ctx->nc_local);
                return -1;
            }
        } else {
            if (probe_open_obmm_mapping(&ctx->cc_peer_import, opts->cc_import_memid,
                                        O_RDWR, PROT_NONE,
                                        opts->cc_offset, ctx->cc_map_len) != 0) {
                probe_close_mapping(&ctx->nc_peer);
                probe_close_mapping(&ctx->nc_local);
                return -1;
            }
        }
    }

    return 0;
}

static void probe_close_context(probe_context_t *ctx)
{
    probe_close_mapping(&ctx->cc_peer_import);
    probe_close_mapping(&ctx->cc_local_export);
    probe_close_mapping(&ctx->nc_peer);
    probe_close_mapping(&ctx->nc_local);
}

static void probe_setup_phase_ctx(const probe_context_t *ctx, probe_mode_t mode,
                                  uint32_t phase_id, probe_phase_ctx_t *phase)
{
    memset(phase, 0, sizeof(*phase));
    phase->mode       = mode;
    phase->name       = probe_mode_name(mode);
    phase->phase_id   = phase_id;
    phase->local_ctrl = (probe_ctrl_t*)ctx->nc_local.base;
    phase->peer_ctrl  = (probe_ctrl_t*)ctx->nc_peer.base;

    if (mode == PROBE_MODE_NC) {
        phase->data_base = (ctx->opts->role == PROBE_ROLE_CLIENT) ?
                           ((uint8_t*)ctx->nc_local.base + ctx->page_size) :
                           ((uint8_t*)ctx->nc_peer.base + ctx->page_size);
        phase->data_fd   = -1;
    } else if (ctx->opts->role == PROBE_ROLE_CLIENT) {
        phase->data_base = (uint8_t*)ctx->cc_local_export.base;
        phase->data_fd   = ctx->cc_local_export.fd;
    } else {
        phase->data_base = (uint8_t*)ctx->cc_peer_import.base;
        phase->data_fd   = ctx->cc_peer_import.fd;
    }
}

static int probe_prepare_phase_data(const probe_context_t *ctx,
                                    const probe_phase_ctx_t *phase)
{
    memset(phase->local_ctrl, 0, ctx->page_size);
    probe_u32_store(&phase->local_ctrl->magic, PROBE_CTRL_MAGIC);
    probe_u32_store(&phase->local_ctrl->run_id, ctx->opts->run_id);
    probe_u32_store(&phase->local_ctrl->phase_id, phase->phase_id);
    probe_u32_store(&phase->local_ctrl->mode, (uint32_t)phase->mode);
    probe_u32_store(&phase->local_ctrl->role, (uint32_t)ctx->opts->role);
    probe_u64_store(&phase->local_ctrl->window_size, ctx->opts->window_size);
    probe_u64_store(&phase->local_ctrl->window_count, ctx->opts->window_count);
    probe_u64_store(&phase->local_ctrl->iters, ctx->opts->iters);
    probe_u64_store(&phase->local_ctrl->warmup, ctx->opts->warmup);

    if ((phase->mode == PROBE_MODE_CC) &&
        (ctx->opts->role == PROBE_ROLE_CLIENT)) {
        probe_cacheable_store_fence();
    }
    probe_bus_store_fence();
    return 0;
}

static int probe_wait_peer_ready(const probe_context_t *ctx,
                                 const probe_phase_ctx_t *phase)
{
    const probe_ctrl_t *peer = phase->peer_ctrl;

    for (;;) {
        if ((probe_u32_load(&peer->magic) == PROBE_CTRL_MAGIC) &&
            (probe_u32_load(&peer->run_id) == ctx->opts->run_id) &&
            (probe_u32_load(&peer->phase_id) == phase->phase_id)) {
            if (probe_u32_load(&peer->error) != 0) {
                errno = (int)probe_u32_load(&peer->error);
                return -1;
            }

            if (probe_u32_load(&peer->ready) != 1) {
                probe_spin_hint();
                continue;
            }

            probe_bus_load_fence();

            if ((probe_u32_load(&peer->mode) != (uint32_t)phase->mode) ||
                (probe_u32_load(&peer->role) == (uint32_t)ctx->opts->role) ||
                (probe_u64_load(&peer->window_size) != ctx->opts->window_size) ||
                (probe_u64_load(&peer->window_count) != ctx->opts->window_count) ||
                (probe_u64_load(&peer->iters) != ctx->opts->iters) ||
                (probe_u64_load(&peer->warmup) != ctx->opts->warmup)) {
                fprintf(stderr,
                        "peer config mismatch for phase=%s: "
                        "peer_mode=%u peer_role=%u peer_window_size=%" PRIu64
                        " peer_window_count=%" PRIu64 " peer_iters=%" PRIu64
                        " peer_warmup=%" PRIu64 "\n",
                        phase->name, probe_u32_load(&peer->mode),
                        probe_u32_load(&peer->role),
                        probe_u64_load(&peer->window_size),
                        probe_u64_load(&peer->window_count),
                        probe_u64_load(&peer->iters),
                        probe_u64_load(&peer->warmup));
                errno = EPROTO;
                return -1;
            }

            return 0;
        }

        probe_spin_hint();
    }
}

static int probe_publish_ready(const probe_context_t *ctx,
                               const probe_phase_ctx_t *phase)
{
    probe_ctrl_t *local = phase->local_ctrl;

    probe_bus_store_fence();
    probe_u32_store(&local->ready, 1);
    return probe_wait_peer_ready(ctx, phase);
}

static int probe_client_progress_acks(const probe_context_t *ctx,
                                      const probe_phase_ctx_t *phase,
                                      uint64_t *reacquired_seq,
                                      probe_stats_t *stats)
{
    uint64_t ack_seq;

    ack_seq = probe_u64_load(&phase->local_ctrl->ack_seq);
    if (ack_seq <= *reacquired_seq) {
        return 0;
    }

    probe_bus_load_fence();
    while (*reacquired_seq < ack_seq) {
        uint64_t seq = *reacquired_seq + 1;

        if (phase->mode == PROBE_MODE_CC) {
            uint8_t *window = phase->data_base +
                              (((size_t)(seq - 1) % ctx->opts->window_count) *
                               ctx->opts->window_size);
            if (probe_set_ownership_timed(phase->data_fd, window,
                                          ctx->opts->window_size, PROT_WRITE,
                                          (seq > ctx->opts->warmup) ?
                                          &stats->tx_reacquire_ns : NULL) != 0) {
                probe_signal_error(phase->local_ctrl, errno);
                return -1;
            }
        }

        *reacquired_seq = seq;
    }

    return 0;
}

static int probe_client_wait_acks_until(const probe_context_t *ctx,
                                        const probe_phase_ctx_t *phase,
                                        uint64_t target_seq,
                                        uint64_t *reacquired_seq,
                                        probe_stats_t *stats,
                                        uint64_t *wait_bucket,
                                        uint64_t *spin_bucket)
{
    uint64_t start_ns;

    start_ns = (wait_bucket != NULL) ? probe_now_ns() : 0;
    for (;;) {
        if (probe_u32_load(&phase->peer_ctrl->error) != 0) {
            errno = (int)probe_u32_load(&phase->peer_ctrl->error);
            return -1;
        }

        if (probe_client_progress_acks(ctx, phase, reacquired_seq, stats) != 0) {
            return -1;
        }

        if (*reacquired_seq >= target_seq) {
            break;
        }

        if (spin_bucket != NULL) {
            (*spin_bucket)++;
        }
        probe_spin_hint();
    }

    if (wait_bucket != NULL) {
        *wait_bucket += probe_now_ns() - start_ns;
    }

    return 0;
}

static int probe_server_wait_req(const probe_phase_ctx_t *phase, uint64_t target_seq,
                                 uint64_t *spins_p)
{
    uint64_t observed;

    for (;;) {
        if (probe_u32_load(&phase->peer_ctrl->error) != 0) {
            errno = (int)probe_u32_load(&phase->peer_ctrl->error);
            return -1;
        }

        observed = probe_u64_load(&phase->peer_ctrl->req_seq);
        if (observed >= target_seq) {
            probe_bus_load_fence();
            return 0;
        }

        if (spins_p != NULL) {
            (*spins_p)++;
        }
        probe_spin_hint();
    }
}

static int probe_client_send_window(const probe_context_t *ctx,
                                    const probe_phase_ctx_t *phase,
                                    uint64_t seq, int measure,
                                    const uint8_t *tx_buf,
                                    probe_stats_t *stats)
{
    uint8_t  *window;
    uint64_t  start_ns;

    window = phase->data_base +
             (((size_t)(seq - 1) % ctx->opts->window_count) *
              ctx->opts->window_size);

    start_ns = probe_now_ns();
    memcpy(window, tx_buf, ctx->opts->window_size);
    if (measure) {
        stats->tx_copy_ns += probe_now_ns() - start_ns;
    }

    if (phase->mode == PROBE_MODE_CC) {
        probe_cacheable_store_fence();
        if (probe_set_ownership_timed(phase->data_fd, window,
                                      ctx->opts->window_size, PROT_READ,
                                      measure ? &stats->tx_release_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
    }

    start_ns = probe_now_ns();
    probe_bus_store_fence();
    probe_u64_store(&phase->local_ctrl->req_seq, seq);
    if (measure) {
        stats->ctrl_publish_ns += probe_now_ns() - start_ns;
    }

    return 0;
}

static int probe_server_recv_window(const probe_context_t *ctx,
                                    const probe_phase_ctx_t *phase,
                                    uint64_t seq, int measure,
                                    uint8_t *rx_buf,
                                    probe_stats_t *stats)
{
    uint8_t  *window;
    uint64_t  start_ns;

    start_ns = probe_now_ns();
    if (probe_server_wait_req(phase, seq, measure ? &stats->req_spins : NULL) != 0) {
        probe_signal_error(phase->local_ctrl, errno);
        return -1;
    }
    if (measure) {
        stats->wait_req_ns += probe_now_ns() - start_ns;
    }

    window = phase->data_base +
             (((size_t)(seq - 1) % ctx->opts->window_count) *
              ctx->opts->window_size);

    if (phase->mode == PROBE_MODE_CC) {
        if (probe_set_ownership_timed(phase->data_fd, window,
                                      ctx->opts->window_size, PROT_READ,
                                      measure ? &stats->rx_acquire_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
        probe_cacheable_load_fence();
    }

    start_ns = probe_now_ns();
    memcpy(rx_buf, window, ctx->opts->window_size);
    if (measure) {
        stats->rx_copy_ns += probe_now_ns() - start_ns;
        stats->checksum   += probe_checksum_bytes(rx_buf, ctx->opts->window_size);
    }

    if (phase->mode == PROBE_MODE_CC) {
        if (probe_set_ownership_timed(phase->data_fd, window,
                                      ctx->opts->window_size, PROT_NONE,
                                      measure ? &stats->rx_release_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
    }

    start_ns = probe_now_ns();
    probe_bus_store_fence();
    /*
     * The sender polls ack_seq in its own export control page. The receiver
     * therefore publishes credits by writing through its import mapping of the
     * sender's export page, i.e. peer_ctrl rather than local_ctrl.
     */
    probe_u64_store(&phase->peer_ctrl->ack_seq, seq);
    if (measure) {
        stats->ctrl_publish_ns += probe_now_ns() - start_ns;
    }

    return 0;
}

static int probe_publish_done(const probe_phase_ctx_t *phase,
                              const probe_stats_t *stats)
{
    memcpy((void*)&phase->local_ctrl->stats, stats, sizeof(*stats));
    probe_bus_store_fence();
    probe_u32_store(&phase->local_ctrl->done, 1);
    return 0;
}

static int probe_wait_peer_done(const probe_phase_ctx_t *phase)
{
    for (;;) {
        if (probe_u32_load(&phase->peer_ctrl->error) != 0) {
            errno = (int)probe_u32_load(&phase->peer_ctrl->error);
            return -1;
        }

        if (probe_u32_load(&phase->peer_ctrl->done) == 1) {
            probe_bus_load_fence();
            return 0;
        }

        probe_spin_hint();
    }
}

static int probe_run_client_phase(const probe_context_t *ctx,
                                  const probe_phase_ctx_t *phase,
                                  probe_stats_t *stats,
                                  probe_phase_result_t *result)
{
    uint8_t  *tx_buf;
    uint64_t  reacquired_seq = 0;
    uint64_t  start_ns;
    uint64_t  end_ns;
    uint64_t  seq;

    tx_buf = malloc(ctx->opts->window_size ? ctx->opts->window_size : 1);
    if (tx_buf == NULL) {
        perror("malloc");
        return -1;
    }

    memset(tx_buf, 0xa5, ctx->opts->window_size);

    for (seq = 1; seq <= ctx->opts->warmup; ++seq) {
        if ((seq > ctx->opts->window_count) &&
            (probe_client_wait_acks_until(ctx, phase, seq - ctx->opts->window_count,
                                          &reacquired_seq, stats, NULL, NULL) != 0)) {
            free(tx_buf);
            return -1;
        }

        if (probe_client_send_window(ctx, phase, seq, 0, tx_buf, stats) != 0) {
            free(tx_buf);
            return -1;
        }
    }

    if (ctx->opts->warmup > 0) {
        if (probe_client_wait_acks_until(ctx, phase, ctx->opts->warmup,
                                         &reacquired_seq, stats, NULL, NULL) != 0) {
            free(tx_buf);
            return -1;
        }
    }

    start_ns = probe_now_ns();
    for (seq = (uint64_t)ctx->opts->warmup + 1;
         seq <= (uint64_t)ctx->opts->warmup + ctx->opts->iters; ++seq) {
        if ((seq > ctx->opts->window_count) &&
            (probe_client_wait_acks_until(ctx, phase, seq - ctx->opts->window_count,
                                          &reacquired_seq, stats,
                                          &stats->wait_credit_ns,
                                          &stats->credit_spins) != 0)) {
            free(tx_buf);
            return -1;
        }

        if (probe_client_send_window(ctx, phase, seq, 1, tx_buf, stats) != 0) {
            free(tx_buf);
            return -1;
        }
    }

    if (probe_client_wait_acks_until(ctx, phase,
                                     (uint64_t)ctx->opts->warmup + ctx->opts->iters,
                                     &reacquired_seq, stats,
                                     &stats->wait_drain_ns,
                                     &stats->drain_spins) != 0) {
        free(tx_buf);
        return -1;
    }
    end_ns = probe_now_ns();

    memset(result, 0, sizeof(*result));
    result->name = phase->name;
    result->gbps = ((double)ctx->opts->iters * ctx->opts->window_size) /
                   (double)(end_ns - start_ns);

    free(tx_buf);
    return 0;
}

static int probe_run_server_phase(const probe_context_t *ctx,
                                  const probe_phase_ctx_t *phase,
                                  probe_stats_t *stats)
{
    uint8_t *rx_buf;
    uint64_t seq;

    rx_buf = malloc(ctx->opts->window_size ? ctx->opts->window_size : 1);
    if (rx_buf == NULL) {
        perror("malloc");
        return -1;
    }

    for (seq = 1; seq <= (uint64_t)ctx->opts->warmup + ctx->opts->iters; ++seq) {
        if (probe_server_recv_window(ctx, phase, seq, seq > ctx->opts->warmup,
                                     rx_buf, stats) != 0) {
            free(rx_buf);
            return -1;
        }
    }

    free(rx_buf);
    return 0;
}

static int probe_run_phase(const probe_context_t *ctx, const probe_phase_ctx_t *phase,
                           probe_phase_result_t *result)
{
    probe_stats_t local_stats;
    probe_stats_t peer_stats;
    int           rc = -1;

    memset(&local_stats, 0, sizeof(local_stats));
    memset(&peer_stats, 0, sizeof(peer_stats));

    if (probe_prepare_phase_data(ctx, phase) != 0) {
        perror("prepare phase data");
        probe_signal_error(phase->local_ctrl, errno);
        return -1;
    }

    if (probe_publish_ready(ctx, phase) != 0) {
        perror("phase ready");
        probe_signal_error(phase->local_ctrl, errno);
        return -1;
    }

    if (ctx->opts->role == PROBE_ROLE_CLIENT) {
        rc = probe_run_client_phase(ctx, phase, &local_stats, result);
    } else {
        rc = probe_run_server_phase(ctx, phase, &local_stats);
    }

    if (rc != 0) {
        probe_signal_error(phase->local_ctrl, errno);
        return -1;
    }

    probe_publish_done(phase, &local_stats);
    if (probe_wait_peer_done(phase) != 0) {
        perror("peer done");
        return -1;
    }

    memcpy(&peer_stats, (const void*)&phase->peer_ctrl->stats, sizeof(peer_stats));

    if (ctx->opts->role == PROBE_ROLE_CLIENT) {
        double sender_own_ns   = (double)(local_stats.tx_release_ns +
                                          local_stats.tx_reacquire_ns) /
                                 ctx->opts->iters;
        double receiver_own_ns = (double)(peer_stats.rx_acquire_ns +
                                          peer_stats.rx_release_ns) /
                                 ctx->opts->iters;

        printf("mode=%s window_size=%zu window_count=%u iters=%u warmup=%u "
               "bytes=%" PRIu64 " throughput_GBps=%.2f "
               "sender_wait_credit_ns=%.2f sender_wait_drain_ns=%.2f "
               "sender_tx_copy_ns=%.2f sender_ctrl_ns=%.2f sender_own_ns=%.2f "
               "receiver_wait_req_ns=%.2f receiver_rx_copy_ns=%.2f "
               "receiver_ctrl_ns=%.2f receiver_own_ns=%.2f\n",
               phase->name, ctx->opts->window_size, ctx->opts->window_count,
               ctx->opts->iters, ctx->opts->warmup,
               (uint64_t)ctx->opts->iters * ctx->opts->window_size, result->gbps,
               (double)local_stats.wait_credit_ns / ctx->opts->iters,
               (double)local_stats.wait_drain_ns / ctx->opts->iters,
               (double)local_stats.tx_copy_ns / ctx->opts->iters,
               (double)local_stats.ctrl_publish_ns / ctx->opts->iters,
               sender_own_ns,
               (double)peer_stats.wait_req_ns / ctx->opts->iters,
               (double)peer_stats.rx_copy_ns / ctx->opts->iters,
               (double)peer_stats.ctrl_publish_ns / ctx->opts->iters,
               receiver_own_ns);

        if (phase->mode == PROBE_MODE_CC) {
            printf("mode=%s_ownership sender_tx_release_ns=%.2f "
                   "sender_tx_reacquire_ns=%.2f receiver_rx_acquire_ns=%.2f "
                   "receiver_rx_release_ns=%.2f\n",
                   phase->name,
                   (double)local_stats.tx_release_ns / ctx->opts->iters,
                   (double)local_stats.tx_reacquire_ns / ctx->opts->iters,
                   (double)peer_stats.rx_acquire_ns / ctx->opts->iters,
                   (double)peer_stats.rx_release_ns / ctx->opts->iters);
        }
    } else {
        printf("role=server mode=%s completed checksum=%" PRIu64 "\n",
               phase->name, local_stats.checksum);
    }

    return 0;
}

static int probe_parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    static const struct option long_opts[] = {
        {"role",            required_argument, NULL, 'r'},
        {"mode",            required_argument, NULL, 'm'},
        {"run-id",          required_argument, NULL, 'u'},
        {"window-size",     required_argument, NULL, 's'},
        {"window-count",    required_argument, NULL, 'W'},
        {"iters",           required_argument, NULL, 'i'},
        {"warmup",          required_argument, NULL, 'w'},
        {"cpu",             required_argument, NULL, 'c'},
        {"nc-export-memid", required_argument, NULL, 'e'},
        {"nc-import-memid", required_argument, NULL, 'n'},
        {"cc-export-memid", required_argument, NULL, 'E'},
        {"cc-import-memid", required_argument, NULL, 'N'},
        {"nc-offset",       required_argument, NULL, 'o'},
        {"cc-offset",       required_argument, NULL, 'O'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    size_t value;
    int    ch;

    memset(opts, 0, sizeof(*opts));
    opts->mode            = PROBE_MODE_BOTH;
    opts->iters           = PROBE_DEFAULT_ITERS;
    opts->warmup          = PROBE_DEFAULT_WARMUP;
    opts->window_size     = PROBE_DEFAULT_WINDOW_SIZE;
    opts->window_count    = PROBE_DEFAULT_WINDOW_COUNT;
    opts->cpu             = -1;
    opts->nc_export_memid = PROBE_DEFAULT_NC_EXPORT;
    opts->nc_import_memid = PROBE_DEFAULT_NC_IMPORT;
    opts->cc_export_memid = PROBE_DEFAULT_CC_EXPORT;
    opts->cc_import_memid = PROBE_DEFAULT_CC_IMPORT;
    opts->nc_offset       = (off_t)PROBE_DEFAULT_NC_OFFSET;
    opts->cc_offset       = (off_t)PROBE_DEFAULT_CC_OFFSET;

    while ((ch = getopt_long(argc, argv, "r:m:u:s:W:i:w:c:e:n:E:N:o:O:h",
                             long_opts, NULL)) != -1) {
        switch (ch) {
        case 'r':
            if (probe_parse_role(optarg, &opts->role) != 0) {
                return -1;
            }
            opts->role_set = 1;
            break;
        case 'm':
            if (probe_parse_mode(optarg, &opts->mode) != 0) {
                return -1;
            }
            break;
        case 'u':
            if (probe_parse_u32(optarg, &opts->run_id) != 0) {
                return -1;
            }
            break;
        case 's':
            if (probe_parse_size(optarg, &value) != 0) {
                return -1;
            }
            opts->window_size = value;
            break;
        case 'W':
            opts->window_count = (unsigned)strtoul(optarg, NULL, 0);
            if (opts->window_count == 0) {
                return -1;
            }
            break;
        case 'i':
            opts->iters = (unsigned)strtoul(optarg, NULL, 0);
            if (opts->iters == 0) {
                return -1;
            }
            break;
        case 'w':
            opts->warmup = (unsigned)strtoul(optarg, NULL, 0);
            break;
        case 'c':
            opts->cpu = atoi(optarg);
            break;
        case 'e':
            if (probe_parse_u64(optarg, &opts->nc_export_memid) != 0) {
                return -1;
            }
            break;
        case 'n':
            if (probe_parse_u64(optarg, &opts->nc_import_memid) != 0) {
                return -1;
            }
            break;
        case 'E':
            if (probe_parse_u64(optarg, &opts->cc_export_memid) != 0) {
                return -1;
            }
            break;
        case 'N':
            if (probe_parse_u64(optarg, &opts->cc_import_memid) != 0) {
                return -1;
            }
            break;
        case 'o':
            if (probe_parse_size(optarg, &value) != 0) {
                return -1;
            }
            opts->nc_offset = (off_t)value;
            break;
        case 'O':
            if (probe_parse_size(optarg, &value) != 0) {
                return -1;
            }
            opts->cc_offset = (off_t)value;
            break;
        case 'h':
        default:
            return -1;
        }
    }

    if (!opts->role_set || (opts->run_id == 0)) {
        return -1;
    }

    return 0;
}

static int probe_validate_opts(const probe_opts_t *opts)
{
    long page_size;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return -1;
    }

    if ((opts->nc_offset < 0) || ((opts->nc_offset % (off_t)page_size) != 0)) {
        fprintf(stderr, "nc-offset must be page-aligned\n");
        return -1;
    }

    if (opts->window_size == 0) {
        fprintf(stderr, "window-size must be non-zero\n");
        return -1;
    }

    if ((opts->mode == PROBE_MODE_CC) || (opts->mode == PROBE_MODE_BOTH)) {
        if ((opts->cc_offset < 0) ||
            ((opts->cc_offset % (off_t)PROBE_DEFAULT_PMD_SIZE) != 0)) {
            fprintf(stderr, "cc-offset must be 2M-aligned\n");
            return -1;
        }

        if ((opts->window_size == 0) ||
            ((opts->window_size % PROBE_DEFAULT_PMD_SIZE) != 0)) {
            fprintf(stderr, "window-size must be a non-zero multiple of 2M for CC\n");
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    probe_opts_t         opts;
    probe_context_t      ctx;
    probe_phase_ctx_t    phase;
    probe_phase_result_t nc_result;
    probe_phase_result_t cc_result;
    int                  rc = 1;

    if (probe_parse_opts(argc, argv, &opts) != 0) {
        probe_usage(argv[0]);
        return 1;
    }

    if (probe_validate_opts(&opts) != 0) {
        return 1;
    }

    if (probe_set_affinity(opts.cpu) != 0) {
        perror("sched_setaffinity");
        return 1;
    }

    if (probe_open_context(&opts, &ctx) != 0) {
        return 1;
    }

    if ((opts.mode == PROBE_MODE_NC) || (opts.mode == PROBE_MODE_BOTH)) {
        probe_setup_phase_ctx(&ctx, PROBE_MODE_NC, 1, &phase);
        if (probe_run_phase(&ctx, &phase, &nc_result) != 0) {
            goto out_close;
        }
    }

    if ((opts.mode == PROBE_MODE_CC) || (opts.mode == PROBE_MODE_BOTH)) {
        probe_setup_phase_ctx(&ctx, PROBE_MODE_CC, 2, &phase);
        if (probe_run_phase(&ctx, &phase, &cc_result) != 0) {
            goto out_close;
        }
    }

    if ((opts.role == PROBE_ROLE_CLIENT) && (opts.mode == PROBE_MODE_BOTH)) {
        printf("compare nc_GBps=%.2f cc_GBps=%.2f cc_vs_nc=%.2f\n",
               nc_result.gbps, cc_result.gbps, cc_result.gbps / nc_result.gbps);
    }

    rc = 0;

out_close:
    probe_close_context(&ctx);
    return rc;
}
