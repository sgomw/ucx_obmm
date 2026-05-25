/*
 * obmm_cross_node_probe.c
 *
 * Cross-node ping-pong probe that compares:
 *   1. NC data path over OBMM shmdev (O_SYNC)
 *   2. CC data path over OBMM shmdev plus the minimum ownership handoff
 *
 * The control plane always uses a small NC region, so NC and CC runs share the
 * same doorbell/ack mechanism. That keeps the comparison focused on the data
 * path itself:
 *   - NC mode: payload + control both use NC mappings
 *   - CC mode: payload uses CC mappings, control still uses NC doorbells, and
 *              the sender/receiver add only the ownership transitions needed
 *              by the OBMM cacheable consistency model
 *
 * CC protocol for one direction:
 *   sender(export):  WRITE payload -> set export WRITE->READ -> ring req
 *   receiver(import):wait req -> set import NONE->READ -> copy payload ->
 *                    set import READ->NONE -> ring ack
 *   sender(export):  wait ack -> set export READ->WRITE
 *
 * This uses four obmm_set_ownership() calls per one-way CC transfer, which is
 * the minimal legal handoff for a ping-pong protocol under the documented
 * "one writer host or all readers" rule.
 *
 * Build:
 *   gcc -O3 -std=gnu11 -Wall -Wextra -o obmm_cross_node_probe \
 *       obmm_cross_node_probe.c -ldl -lrt
 *
 * Example (run the same command on both nodes, changing only --role):
 *   ./obmm_cross_node_probe --role server --mode both --run-id 7 \
 *       --nc-export-memid 1 --nc-import-memid 2 \
 *       --cc-export-memid 3 --cc-import-memid 4
 *
 *   ./obmm_cross_node_probe --role client --mode both --run-id 7 \
 *       --nc-export-memid 1 --nc-import-memid 2 \
 *       --cc-export-memid 3 --cc-import-memid 4
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

#define PROBE_CTRL_MAGIC             0x504d4d4fu
#define PROBE_DEFAULT_ITERS          50000u
#define PROBE_DEFAULT_WARMUP         5000u
#define PROBE_DEFAULT_SIZE           1u
#define PROBE_DEFAULT_CC_WINDOW      (2ul * 1024ul * 1024ul)
#define PROBE_DEFAULT_NC_OFFSET      0ul
#define PROBE_DEFAULT_CC_OFFSET      0ul
#define PROBE_MAX_SIZE               (1024ul * 1024ul)
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
    uint64_t wait_req_ns;
    uint64_t wait_ack_ns;
    uint64_t tx_copy_ns;
    uint64_t rx_copy_ns;
    uint64_t ctrl_publish_ns;
    uint64_t tx_release_ns;
    uint64_t rx_acquire_ns;
    uint64_t rx_release_ns;
    uint64_t tx_reacquire_ns;
    uint64_t req_spins;
    uint64_t ack_spins;
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
    volatile uint32_t msg_size;
    volatile uint32_t iters;
    volatile uint32_t warmup;
    volatile uint32_t req_seq;
    volatile uint32_t ack_seq;
    volatile uint32_t error;
    volatile uint32_t reserved;
    probe_stats_t     stats;
} probe_ctrl_t;

typedef struct probe_opts {
    probe_role_t role;
    probe_mode_t mode;
    int          role_set;
    uint32_t     run_id;
    unsigned     iters;
    unsigned     warmup;
    size_t       msg_size;
    int          cpu;
    uint64_t     nc_export_memid;
    uint64_t     nc_import_memid;
    uint64_t     cc_export_memid;
    uint64_t     cc_import_memid;
    off_t        nc_offset;
    off_t        cc_offset;
    size_t       cc_window;
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
    size_t              nc_window;
    size_t              nc_map_len;
    size_t              cc_map_len;
    probe_mapping_t     nc_export;
    probe_mapping_t     nc_import;
    probe_mapping_t     cc_export;
    probe_mapping_t     cc_import;
} probe_context_t;

typedef struct probe_phase_ctx {
    probe_mode_t   mode;
    const char    *name;
    uint32_t       phase_id;
    probe_ctrl_t  *local_ctrl;
    probe_ctrl_t  *peer_ctrl;
    uint8_t       *local_data;
    uint8_t       *peer_data;
    size_t         data_len;
    int            local_fd;
    int            peer_fd;
} probe_phase_ctx_t;

typedef struct probe_phase_result {
    const char *name;
    double      half_rtt_ns;
} probe_phase_result_t;

static void probe_usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Required options:\n"
            "  --role <server|client>\n"
            "  --run-id <u32>\n"
            "  --nc-export-memid <id>\n"
            "  --nc-import-memid <id>\n"
            "\n"
            "CC-only options (required for --mode cc|both):\n"
            "  --cc-export-memid <id>\n"
            "  --cc-import-memid <id>\n"
            "\n"
            "Other options:\n"
            "  --mode <nc|cc|both>        Benchmark mode (default: both)\n"
            "  --size <bytes>             Payload bytes per direction (default: %u)\n"
            "  --iters <count>            Measured iterations (default: %u)\n"
            "  --warmup <count>           Warmup iterations (default: %u)\n"
            "  --cpu <cpu>                Pin process to one CPU\n"
            "  --nc-offset <bytes>        NC control-page offset (default: 0)\n"
            "  --cc-offset <bytes>        CC data-window offset (default: 0)\n"
            "  --cc-window <bytes>        CC ownership window (default: 2M)\n"
            "\n"
            "Notes:\n"
            "  - The NC region always carries the control plane.\n"
            "  - NC payload starts one page after --nc-offset.\n"
            "  - CC payload uses the range [--cc-offset, --cc-offset + --cc-window).\n"
            "  - --run-id should be changed between independent runs to avoid stale\n"
            "    control-page state from an earlier run.\n",
            progname, PROBE_DEFAULT_SIZE, PROBE_DEFAULT_ITERS,
            PROBE_DEFAULT_WARMUP);
}

static uint64_t probe_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + ts.tv_nsec;
}

static size_t probe_align_up(size_t value, size_t align)
{
    return (value + align - 1) & ~(align - 1);
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

static inline uint32_t probe_word_load(const volatile uint32_t *ptr)
{
    return *ptr;
}

static probe_obmm_set_ownership_func_t probe_resolve_obmm_set_ownership(void)
{
    static probe_obmm_set_ownership_func_t func;
    static int                             resolved;
    static void                           *handles[3];
    const char                            *env_path;
    int                                    i;
    const char                            *candidates[2];

    if (resolved) {
        return func;
    }

    resolved = 1;
    func = (probe_obmm_set_ownership_func_t)dlsym(RTLD_DEFAULT,
                                                  "obmm_set_ownership");
    if (func != NULL) {
        return func;
    }

    env_path       = getenv("OBMM_LIBOBMM_PATH");
    candidates[0]  = "libobmm.so";
    candidates[1]  = "libobmm.so.0";

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

static inline void probe_word_store(volatile uint32_t *ptr, uint32_t value)
{
    *ptr = value;
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
    probe_word_store(&local_ctrl->error, (uint32_t)((err != 0) ? err : EIO));
    probe_bus_store_fence();
    probe_word_store(&local_ctrl->done, 1);
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

static int probe_open_context(const probe_opts_t *opts, probe_context_t *ctx)
{
    size_t payload_floor;
    long   page_size;

    memset(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return -1;
    }

    ctx->page_size = (size_t)page_size;
    payload_floor  = (opts->msg_size > 0) ? opts->msg_size : 1;
    ctx->nc_window = probe_align_up(payload_floor, ctx->page_size);
    ctx->nc_map_len = ctx->page_size + ctx->nc_window;

    if (probe_open_obmm_mapping(&ctx->nc_export, opts->nc_export_memid,
                                O_RDWR | O_SYNC, PROT_READ | PROT_WRITE,
                                opts->nc_offset, ctx->nc_map_len) != 0) {
        return -1;
    }

    if (probe_open_obmm_mapping(&ctx->nc_import, opts->nc_import_memid,
                                O_RDWR | O_SYNC, PROT_READ | PROT_WRITE,
                                opts->nc_offset, ctx->nc_map_len) != 0) {
        probe_close_mapping(&ctx->nc_export);
        return -1;
    }

    if ((opts->mode == PROBE_MODE_CC) || (opts->mode == PROBE_MODE_BOTH)) {
        ctx->cc_map_len = opts->cc_window;
        if (probe_open_obmm_mapping(&ctx->cc_export, opts->cc_export_memid,
                                    O_RDWR, PROT_READ | PROT_WRITE,
                                    opts->cc_offset, ctx->cc_map_len) != 0) {
            probe_close_mapping(&ctx->nc_import);
            probe_close_mapping(&ctx->nc_export);
            return -1;
        }

        if (probe_open_obmm_mapping(&ctx->cc_import, opts->cc_import_memid,
                                    O_RDWR, PROT_NONE,
                                    opts->cc_offset, ctx->cc_map_len) != 0) {
            probe_close_mapping(&ctx->cc_export);
            probe_close_mapping(&ctx->nc_import);
            probe_close_mapping(&ctx->nc_export);
            return -1;
        }
    }

    return 0;
}

static void probe_close_context(probe_context_t *ctx)
{
    probe_close_mapping(&ctx->cc_import);
    probe_close_mapping(&ctx->cc_export);
    probe_close_mapping(&ctx->nc_import);
    probe_close_mapping(&ctx->nc_export);
}

static void probe_setup_phase_ctx(const probe_context_t *ctx, probe_mode_t mode,
                                  uint32_t phase_id, probe_phase_ctx_t *phase)
{
    memset(phase, 0, sizeof(*phase));
    phase->mode       = mode;
    phase->name       = probe_mode_name(mode);
    phase->phase_id   = phase_id;
    phase->local_ctrl = (probe_ctrl_t*)ctx->nc_export.base;
    phase->peer_ctrl  = (probe_ctrl_t*)ctx->nc_import.base;

    if (mode == PROBE_MODE_NC) {
        phase->local_data = (uint8_t*)ctx->nc_export.base + ctx->page_size;
        phase->peer_data  = (uint8_t*)ctx->nc_import.base + ctx->page_size;
        phase->data_len   = ctx->nc_window;
        phase->local_fd   = -1;
        phase->peer_fd    = -1;
    } else {
        phase->local_data = (uint8_t*)ctx->cc_export.base;
        phase->peer_data  = (uint8_t*)ctx->cc_import.base;
        phase->data_len   = ctx->opts->cc_window;
        phase->local_fd   = ctx->cc_export.fd;
        phase->peer_fd    = ctx->cc_import.fd;
    }
}

static int probe_set_ownership_timed(int fd, void *start, size_t length, int prot,
                                     uint64_t *bucket)
{
    probe_obmm_set_ownership_func_t set_ownership;
    uint64_t start_ns;

    start_ns = 0;
    if (bucket != NULL) {
        start_ns = probe_now_ns();
    }

    set_ownership = probe_resolve_obmm_set_ownership();
    if (set_ownership == NULL) {
        fprintf(stderr,
                "failed to resolve obmm_set_ownership; "
                "set OBMM_LIBOBMM_PATH or put libobmm.so in the loader path\n");
        errno = ENOSYS;
        return -1;
    }

    if (set_ownership(fd, start, (void*)((uintptr_t)start + length), prot) != 0) {
        return -1;
    }

    if (bucket != NULL) {
        *bucket += probe_now_ns() - start_ns;
    }

    return 0;
}

static int probe_prepare_phase_data(const probe_phase_ctx_t *phase,
                                    const probe_context_t *ctx)
{
    size_t zero_len;

    memset(phase->local_ctrl, 0, ctx->page_size);
    zero_len = (ctx->opts->msg_size > 0) ? ctx->opts->msg_size : 1;

    if (phase->mode == PROBE_MODE_NC) {
        memset(phase->local_data, 0, zero_len);
        probe_bus_store_fence();
        return 0;
    }

    if (probe_set_ownership_timed(phase->local_fd, phase->local_data,
                                  phase->data_len, PROT_WRITE, NULL) != 0) {
        return -1;
    }

    if (probe_set_ownership_timed(phase->peer_fd, phase->peer_data,
                                  phase->data_len, PROT_NONE, NULL) != 0) {
        return -1;
    }

    memset(phase->local_data, 0, zero_len);
    probe_cacheable_store_fence();
    probe_bus_store_fence();
    return 0;
}

static int probe_wait_peer_ready(const probe_context_t *ctx,
                                 const probe_phase_ctx_t *phase)
{
    const probe_ctrl_t *peer = phase->peer_ctrl;

    for (;;) {
        if (probe_word_load(&peer->error) != 0) {
            errno = (int)probe_word_load(&peer->error);
            return -1;
        }

        if ((probe_word_load(&peer->magic) == PROBE_CTRL_MAGIC) &&
            (probe_word_load(&peer->run_id) == ctx->opts->run_id) &&
            (probe_word_load(&peer->phase_id) == phase->phase_id) &&
            (probe_word_load(&peer->ready) == 1)) {
            probe_bus_load_fence();

            if ((probe_word_load(&peer->mode) != (uint32_t)phase->mode) ||
                (probe_word_load(&peer->role) == (uint32_t)ctx->opts->role) ||
                (probe_word_load(&peer->msg_size) != ctx->opts->msg_size) ||
                (probe_word_load(&peer->iters) != ctx->opts->iters) ||
                (probe_word_load(&peer->warmup) != ctx->opts->warmup)) {
                fprintf(stderr,
                        "peer config mismatch for phase=%s: "
                        "peer_mode=%u peer_role=%u peer_size=%u peer_iters=%u peer_warmup=%u\n",
                        phase->name, probe_word_load(&peer->mode),
                        probe_word_load(&peer->role),
                        probe_word_load(&peer->msg_size),
                        probe_word_load(&peer->iters),
                        probe_word_load(&peer->warmup));
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

    probe_word_store(&local->magic, PROBE_CTRL_MAGIC);
    probe_word_store(&local->run_id, ctx->opts->run_id);
    probe_word_store(&local->phase_id, phase->phase_id);
    probe_word_store(&local->mode, (uint32_t)phase->mode);
    probe_word_store(&local->role, (uint32_t)ctx->opts->role);
    probe_word_store(&local->msg_size, (uint32_t)ctx->opts->msg_size);
    probe_word_store(&local->iters, ctx->opts->iters);
    probe_word_store(&local->warmup, ctx->opts->warmup);
    probe_bus_store_fence();
    probe_word_store(&local->ready, 1);
    return probe_wait_peer_ready(ctx, phase);
}

static int probe_wait_remote_seq(volatile uint32_t *seq_ptr, uint32_t target,
                                 const probe_ctrl_t *peer_ctrl, uint64_t *spins_p)
{
    for (;;) {
        if (probe_word_load((volatile uint32_t*)&peer_ctrl->error) != 0) {
            errno = (int)probe_word_load((volatile uint32_t*)&peer_ctrl->error);
            return -1;
        }

        if (probe_word_load(seq_ptr) == target) {
            return 0;
        }

        if (spins_p != NULL) {
            (*spins_p)++;
        }
        probe_spin_hint();
    }
}

static int probe_send_one(const probe_context_t *ctx, const probe_phase_ctx_t *phase,
                          uint32_t seq, int measure, const uint8_t *tx_buf,
                          probe_stats_t *stats)
{
    uint64_t start_ns;

    if (ctx->opts->msg_size > 0) {
        start_ns = probe_now_ns();
        memcpy(phase->local_data, tx_buf, ctx->opts->msg_size);
        if (measure) {
            stats->tx_copy_ns += probe_now_ns() - start_ns;
        }
    }

    if (phase->mode == PROBE_MODE_CC) {
        probe_cacheable_store_fence();
        if (probe_set_ownership_timed(phase->local_fd, phase->local_data,
                                      phase->data_len, PROT_READ,
                                      measure ? &stats->tx_release_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
    }

    start_ns = probe_now_ns();
    probe_bus_store_fence();
    probe_word_store(&phase->local_ctrl->req_seq, seq);
    if (measure) {
        stats->ctrl_publish_ns += probe_now_ns() - start_ns;
    }

    start_ns = probe_now_ns();
    if (probe_wait_remote_seq(&phase->local_ctrl->ack_seq, seq,
                              phase->peer_ctrl,
                              measure ? &stats->ack_spins : NULL) != 0) {
        probe_signal_error(phase->local_ctrl, errno);
        return -1;
    }
    if (measure) {
        stats->wait_ack_ns += probe_now_ns() - start_ns;
    }
    probe_bus_load_fence();

    if (phase->mode == PROBE_MODE_CC) {
        if (probe_set_ownership_timed(phase->local_fd, phase->local_data,
                                      phase->data_len, PROT_WRITE,
                                      measure ? &stats->tx_reacquire_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
    }

    return 0;
}

static int probe_recv_one(const probe_context_t *ctx, const probe_phase_ctx_t *phase,
                          uint32_t seq, int measure, uint8_t *rx_buf,
                          probe_stats_t *stats)
{
    uint64_t start_ns;

    start_ns = probe_now_ns();
    if (probe_wait_remote_seq(&phase->peer_ctrl->req_seq, seq,
                              phase->peer_ctrl,
                              measure ? &stats->req_spins : NULL) != 0) {
        probe_signal_error(phase->local_ctrl, errno);
        return -1;
    }
    if (measure) {
        stats->wait_req_ns += probe_now_ns() - start_ns;
    }
    probe_bus_load_fence();

    if (phase->mode == PROBE_MODE_CC) {
        if (probe_set_ownership_timed(phase->peer_fd, phase->peer_data,
                                      phase->data_len, PROT_READ,
                                      measure ? &stats->rx_acquire_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
        probe_cacheable_load_fence();
    }

    if (ctx->opts->msg_size > 0) {
        start_ns = probe_now_ns();
        memcpy(rx_buf, phase->peer_data, ctx->opts->msg_size);
        if (measure) {
            stats->rx_copy_ns += probe_now_ns() - start_ns;
            stats->checksum   += probe_checksum_bytes(rx_buf, ctx->opts->msg_size);
        }
    }

    if (phase->mode == PROBE_MODE_CC) {
        if (probe_set_ownership_timed(phase->peer_fd, phase->peer_data,
                                      phase->data_len, PROT_NONE,
                                      measure ? &stats->rx_release_ns : NULL) != 0) {
            probe_signal_error(phase->local_ctrl, errno);
            return -1;
        }
    }

    start_ns = probe_now_ns();
    probe_bus_store_fence();
    probe_word_store(&phase->peer_ctrl->ack_seq, seq);
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
    probe_word_store(&phase->local_ctrl->done, 1);
    return 0;
}

static int probe_wait_peer_done(const probe_phase_ctx_t *phase)
{
    for (;;) {
        if (probe_word_load(&phase->peer_ctrl->error) != 0) {
            errno = (int)probe_word_load(&phase->peer_ctrl->error);
            return -1;
        }

        if (probe_word_load(&phase->peer_ctrl->done) == 1) {
            probe_bus_load_fence();
            return 0;
        }

        probe_spin_hint();
    }
}

static int probe_run_phase(const probe_context_t *ctx, const probe_phase_ctx_t *phase,
                           probe_phase_result_t *result)
{
    probe_stats_t  local_stats;
    probe_stats_t  peer_stats;
    uint8_t       *tx_buf;
    uint8_t       *rx_buf;
    unsigned       total_iters;
    uint64_t       start_ns = 0;
    uint64_t       end_ns   = 0;
    unsigned       i;
    int            rc = -1;

    memset(&local_stats, 0, sizeof(local_stats));
    memset(&peer_stats, 0, sizeof(peer_stats));

    tx_buf = malloc(ctx->opts->msg_size ? ctx->opts->msg_size : 1);
    rx_buf = malloc(ctx->opts->msg_size ? ctx->opts->msg_size : 1);
    if ((tx_buf == NULL) || (rx_buf == NULL)) {
        perror("malloc");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    memset(tx_buf, (ctx->opts->role == PROBE_ROLE_CLIENT) ? 0xa5 : 0x5a,
           ctx->opts->msg_size);

    if (probe_prepare_phase_data(phase, ctx) != 0) {
        perror("prepare phase data");
        goto out_free;
    }

    if (probe_publish_ready(ctx, phase) != 0) {
        perror("phase ready");
        probe_signal_error(phase->local_ctrl, errno);
        goto out_free;
    }

    total_iters = ctx->opts->warmup + ctx->opts->iters;
    for (i = 1; i <= total_iters; ++i) {
        int measure = (i > ctx->opts->warmup);

        if ((ctx->opts->role == PROBE_ROLE_CLIENT) && (i == (ctx->opts->warmup + 1))) {
            start_ns = probe_now_ns();
        }

        if (ctx->opts->role == PROBE_ROLE_CLIENT) {
            if (probe_send_one(ctx, phase, i, measure, tx_buf, &local_stats) != 0) {
                goto out_free;
            }
            if (probe_recv_one(ctx, phase, i, measure, rx_buf, &local_stats) != 0) {
                goto out_free;
            }
        } else {
            if (probe_recv_one(ctx, phase, i, measure, rx_buf, &local_stats) != 0) {
                goto out_free;
            }
            if (probe_send_one(ctx, phase, i, measure, tx_buf, &local_stats) != 0) {
                goto out_free;
            }
        }
    }

    if (ctx->opts->role == PROBE_ROLE_CLIENT) {
        end_ns = probe_now_ns();
    }

    probe_publish_done(phase, &local_stats);
    if (probe_wait_peer_done(phase) != 0) {
        perror("peer done");
        goto out_free;
    }

    memcpy(&peer_stats, (const void*)&phase->peer_ctrl->stats, sizeof(peer_stats));
    memset(result, 0, sizeof(*result));
    result->name = phase->name;
    if (ctx->opts->role == PROBE_ROLE_CLIENT) {
        double client_own_ns = (double)(local_stats.tx_release_ns +
                                        local_stats.rx_acquire_ns +
                                        local_stats.rx_release_ns +
                                        local_stats.tx_reacquire_ns) /
                               ctx->opts->iters;
        double server_own_ns = (double)(peer_stats.tx_release_ns +
                                        peer_stats.rx_acquire_ns +
                                        peer_stats.rx_release_ns +
                                        peer_stats.tx_reacquire_ns) /
                               ctx->opts->iters;

        result->half_rtt_ns = (double)(end_ns - start_ns) / ctx->opts->iters / 2.0;

        printf("mode=%s size=%zu iters=%u warmup=%u half_rtt_ns=%.2f "
               "client_wait_req_ns=%.2f client_wait_ack_ns=%.2f "
               "client_tx_copy_ns=%.2f client_rx_copy_ns=%.2f "
               "client_ctrl_ns=%.2f client_own_ns=%.2f "
               "server_wait_req_ns=%.2f server_wait_ack_ns=%.2f "
               "server_tx_copy_ns=%.2f server_rx_copy_ns=%.2f "
               "server_ctrl_ns=%.2f server_own_ns=%.2f\n",
               phase->name, ctx->opts->msg_size, ctx->opts->iters, ctx->opts->warmup,
               result->half_rtt_ns,
               (double)local_stats.wait_req_ns / ctx->opts->iters,
               (double)local_stats.wait_ack_ns / ctx->opts->iters,
               (double)local_stats.tx_copy_ns / ctx->opts->iters,
               (double)local_stats.rx_copy_ns / ctx->opts->iters,
               (double)local_stats.ctrl_publish_ns / ctx->opts->iters,
               client_own_ns,
               (double)peer_stats.wait_req_ns / ctx->opts->iters,
               (double)peer_stats.wait_ack_ns / ctx->opts->iters,
               (double)peer_stats.tx_copy_ns / ctx->opts->iters,
               (double)peer_stats.rx_copy_ns / ctx->opts->iters,
               (double)peer_stats.ctrl_publish_ns / ctx->opts->iters,
               server_own_ns);

        if (phase->mode == PROBE_MODE_CC) {
            printf("mode=%s_ownership client_tx_release_ns=%.2f "
                   "client_rx_acquire_ns=%.2f client_rx_release_ns=%.2f "
                   "client_tx_reacquire_ns=%.2f "
                   "server_tx_release_ns=%.2f server_rx_acquire_ns=%.2f "
                   "server_rx_release_ns=%.2f server_tx_reacquire_ns=%.2f\n",
                   phase->name,
                   (double)local_stats.tx_release_ns / ctx->opts->iters,
                   (double)local_stats.rx_acquire_ns / ctx->opts->iters,
                   (double)local_stats.rx_release_ns / ctx->opts->iters,
                   (double)local_stats.tx_reacquire_ns / ctx->opts->iters,
                   (double)peer_stats.tx_release_ns / ctx->opts->iters,
                   (double)peer_stats.rx_acquire_ns / ctx->opts->iters,
                   (double)peer_stats.rx_release_ns / ctx->opts->iters,
                   (double)peer_stats.tx_reacquire_ns / ctx->opts->iters);
        }
    } else {
        printf("role=server mode=%s completed checksum=%" PRIu64 "\n",
               phase->name, local_stats.checksum);
    }

    rc = 0;

out_free:
    free(tx_buf);
    free(rx_buf);
    return rc;
}

static int probe_parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    static const struct option long_opts[] = {
        {"role",            required_argument, NULL, 'r'},
        {"mode",            required_argument, NULL, 'm'},
        {"run-id",          required_argument, NULL, 'u'},
        {"size",            required_argument, NULL, 's'},
        {"iters",           required_argument, NULL, 'i'},
        {"warmup",          required_argument, NULL, 'w'},
        {"cpu",             required_argument, NULL, 'c'},
        {"nc-export-memid", required_argument, NULL, 'e'},
        {"nc-import-memid", required_argument, NULL, 'n'},
        {"cc-export-memid", required_argument, NULL, 'E'},
        {"cc-import-memid", required_argument, NULL, 'N'},
        {"nc-offset",       required_argument, NULL, 'o'},
        {"cc-offset",       required_argument, NULL, 'O'},
        {"cc-window",       required_argument, NULL, 'W'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    size_t value;
    int    ch;

    memset(opts, 0, sizeof(*opts));
    opts->mode      = PROBE_MODE_BOTH;
    opts->iters     = PROBE_DEFAULT_ITERS;
    opts->warmup    = PROBE_DEFAULT_WARMUP;
    opts->msg_size  = PROBE_DEFAULT_SIZE;
    opts->cpu       = -1;
    opts->nc_offset = (off_t)PROBE_DEFAULT_NC_OFFSET;
    opts->cc_offset = (off_t)PROBE_DEFAULT_CC_OFFSET;
    opts->cc_window = PROBE_DEFAULT_CC_WINDOW;

    while ((ch = getopt_long(argc, argv, "r:m:u:s:i:w:c:e:n:E:N:o:O:W:h",
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
            if ((probe_parse_size(optarg, &value) != 0) ||
                (value > PROBE_MAX_SIZE)) {
                return -1;
            }
            opts->msg_size = value;
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
        case 'W':
            if (probe_parse_size(optarg, &value) != 0) {
                return -1;
            }
            opts->cc_window = value;
            break;
        case 'h':
        default:
            return -1;
        }
    }

    if (!opts->role_set || (opts->run_id == 0) ||
        (opts->nc_export_memid == 0) ||
        (opts->nc_import_memid == 0)) {
        return -1;
    }

    if ((opts->mode == PROBE_MODE_CC) || (opts->mode == PROBE_MODE_BOTH)) {
        if ((opts->cc_export_memid == 0) || (opts->cc_import_memid == 0)) {
            return -1;
        }
    }

    return 0;
}

static int probe_validate_opts(const probe_opts_t *opts)
{
    if ((opts->nc_offset < 0) || ((opts->nc_offset % (off_t)sysconf(_SC_PAGESIZE)) != 0)) {
        fprintf(stderr, "nc-offset must be page-aligned\n");
        return -1;
    }

    if ((opts->mode == PROBE_MODE_CC) || (opts->mode == PROBE_MODE_BOTH)) {
        if (opts->msg_size > opts->cc_window) {
            fprintf(stderr, "size=%zu exceeds cc-window=%zu\n",
                    opts->msg_size, opts->cc_window);
            return -1;
        }

        if ((opts->cc_offset < 0) ||
            ((opts->cc_offset % (off_t)PROBE_DEFAULT_CC_WINDOW) != 0)) {
            fprintf(stderr, "cc-offset must be 2M-aligned\n");
            return -1;
        }

        if ((opts->cc_window == 0) ||
            ((opts->cc_window % PROBE_DEFAULT_CC_WINDOW) != 0)) {
            fprintf(stderr, "cc-window must be a non-zero multiple of 2M\n");
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
        printf("compare nc_half_rtt_ns=%.2f cc_half_rtt_ns=%.2f cc_vs_nc=%.2f\n",
               nc_result.half_rtt_ns, cc_result.half_rtt_ns,
               cc_result.half_rtt_ns / nc_result.half_rtt_ns);
    }

    rc = 0;

out_close:
    probe_close_context(&ctx);
    return rc;
}
