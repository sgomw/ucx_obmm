/*
 * obmm_posix_same_node_probe.c
 *
 * Minimal same-node ping-pong probe to compare the raw floor of:
 *   1. cacheable POSIX shared memory
 *   2. OBMM shmdev mapped with O_SYNC (NC)
 *
 * The program intentionally avoids UCX/MPI and uses the same two-process,
 * two-mailbox SPSC protocol in both modes, so the delta is dominated by the
 * mapping/coherence/fence model rather than higher-level software.
 *
 * Build:
 *   gcc -O3 -std=gnu11 -Wall -Wextra -o obmm_posix_same_node_probe \
 *       obmm_posix_same_node_probe.c -lrt
 *
 * Examples:
 *   ./obmm_posix_same_node_probe --mode both --size 1 --iters 200000
 *   ./obmm_posix_same_node_probe --mode obmm --size 0 --cpu-parent 0 --cpu-child 1
 *   ./obmm_posix_same_node_probe --mode obmm --obmm-dev /dev/obmm_shmdev123 \
 *       --obmm-offset 120M
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROBE_SYSFS_ROOT          "/sys/devices/obmm"
#define PROBE_SHMDEV_PREFIX       "obmm_shmdev"
#define PROBE_DEFAULT_ITERS       200000u
#define PROBE_DEFAULT_WARMUP      20000u
#define PROBE_DEFAULT_SIZE        1u
#define PROBE_MAX_SIZE            4096u
#define PROBE_DEFAULT_MAP_LEN     (2ul * 1024ul * 1024ul)
#define PROBE_DEFAULT_OBMM_OFFSET (120ul * 1024ul * 1024ul)
#define PROBE_CACHELINE           64u

typedef enum {
    PROBE_MODE_POSIX = 0,
    PROBE_MODE_OBMM  = 1,
    PROBE_MODE_BOTH  = 2
} probe_mode_t;

typedef enum {
    PROBE_FENCE_CACHEABLE = 0,
    PROBE_FENCE_BUS       = 1
} probe_fence_t;

typedef struct __attribute__((aligned(PROBE_CACHELINE))) probe_proc_stats {
    uint64_t wait_ns;
    uint64_t copy_ns;
    uint64_t publish_ns;
    uint64_t spins;
    uint64_t checksum;
} probe_proc_stats_t;

typedef struct __attribute__((aligned(PROBE_CACHELINE))) probe_mailbox {
    volatile uint32_t seq;
    uint8_t           reserved[PROBE_CACHELINE - sizeof(uint32_t)];
    uint8_t          payload[PROBE_MAX_SIZE];
} probe_mailbox_t;

typedef struct __attribute__((aligned(PROBE_CACHELINE))) probe_shared {
    volatile uint32_t start;
    volatile uint32_t child_ready;
    uint8_t           reserved[PROBE_CACHELINE - (2 * sizeof(uint32_t))];
    probe_proc_stats_t parent_stats;
    probe_proc_stats_t child_stats;
    probe_mailbox_t    to_child;
    probe_mailbox_t    to_parent;
} probe_shared_t;

typedef struct probe_result {
    const char       *mode_name;
    double            half_rtt_ns;
    double            parent_wait_ns;
    double            parent_copy_ns;
    double            parent_publish_ns;
    double            parent_spins;
    double            child_wait_ns;
    double            child_copy_ns;
    double            child_publish_ns;
    double            child_spins;
    uint64_t          parent_checksum;
    uint64_t          child_checksum;
} probe_result_t;

typedef struct probe_opts {
    probe_mode_t mode;
    unsigned     iters;
    unsigned     warmup;
    size_t       msg_size;
    size_t       map_len;
    off_t        obmm_offset;
    int          cpu_parent;
    int          cpu_child;
    char         obmm_dev[256];
    int          obmm_dev_set;
} probe_opts_t;

typedef struct probe_mapping {
    void  *base;
    size_t length;
    int    fd;
    char   shm_name[128];
    int    is_posix;
} probe_mapping_t;

static void probe_usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  --mode <posix|obmm|both>   Benchmark mode (default: both)\n"
            "  --size <bytes>             Payload bytes per direction (default: 1, max: %u)\n"
            "  --iters <count>            Measured iterations (default: %u)\n"
            "  --warmup <count>           Warmup iterations (default: %u)\n"
            "  --cpu-parent <cpu>         Pin parent to CPU\n"
            "  --cpu-child <cpu>          Pin child to CPU\n"
            "  --obmm-dev <path>          Use explicit /dev/obmm_shmdevX path\n"
            "  --obmm-offset <bytes>      Offset inside obmm region (default: 120M)\n"
            "  --map-len <bytes>          Mapped bytes for obmm/posix region (default: 2M)\n"
            "\n"
            "Notes:\n"
            "  - The default obmm offset is chosen to stay above the current UCX pool\n"
            "    footprint (~68.5 MiB in the 128 MiB baseline).\n"
            "  - size=0 measures pure control/fence/poll overhead.\n",
            progname, PROBE_MAX_SIZE, PROBE_DEFAULT_ITERS, PROBE_DEFAULT_WARMUP);
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

static ssize_t probe_read_file(char *buf, size_t max, const char *path)
{
    int     fd;
    ssize_t nread;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    nread = read(fd, buf, max - 1);
    close(fd);
    if (nread < 0) {
        return -1;
    }

    buf[nread] = '\0';
    while ((nread > 0) &&
           ((buf[nread - 1] == '\n') || (buf[nread - 1] == '\r') ||
            (buf[nread - 1] == ' ') || (buf[nread - 1] == '\t'))) {
        buf[--nread] = '\0';
    }

    return nread;
}

static int probe_parse_memid(const char *name, uint64_t *memid_p)
{
    const size_t prefix_len = sizeof(PROBE_SHMDEV_PREFIX) - 1;
    char        *end;
    uint64_t     memid;

    if (strncmp(name, PROBE_SHMDEV_PREFIX, prefix_len) != 0) {
        return 0;
    }

    if (name[prefix_len] == '\0') {
        return 0;
    }

    errno = 0;
    memid = strtoull(name + prefix_len, &end, 10);
    if ((errno != 0) || (*end != '\0') || (memid == 0)) {
        return 0;
    }

    *memid_p = memid;
    return 1;
}

static int probe_discover_obmm_export(char *dev_path, size_t dev_path_len)
{
    DIR           *dir;
    struct dirent *entry;
    char           path[512];
    char           type_buf[32];
    char           mmap_buf[32];
    uint64_t       memid;

    dir = opendir(PROBE_SYSFS_ROOT);
    if (dir == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!probe_parse_memid(entry->d_name, &memid)) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s/type", PROBE_SYSFS_ROOT,
                 entry->d_name);
        if (probe_read_file(type_buf, sizeof(type_buf), path) < 0) {
            continue;
        }

        if (strcmp(type_buf, "export") != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s/allow_mmap", PROBE_SYSFS_ROOT,
                 entry->d_name);
        if (probe_read_file(mmap_buf, sizeof(mmap_buf), path) < 0) {
            continue;
        }

        if (strtol(mmap_buf, NULL, 0) == 0) {
            continue;
        }

        snprintf(dev_path, dev_path_len, "/dev/%s", entry->d_name);
        closedir(dir);
        return 0;
    }

    closedir(dir);
    return -1;
}

static inline void probe_cacheable_store_fence(void)
{
    atomic_thread_fence(memory_order_release);
}

static inline void probe_cacheable_load_fence(void)
{
    atomic_thread_fence(memory_order_acquire);
}

static inline void probe_bus_store_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("sfence" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb oshst" ::: "memory");
#else
    atomic_thread_fence(memory_order_release);
#endif
}

static inline void probe_bus_load_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("lfence" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb oshld" ::: "memory");
#else
    atomic_thread_fence(memory_order_acquire);
#endif
}

static inline void probe_store_fence(probe_fence_t fence)
{
    if (fence == PROBE_FENCE_BUS) {
        probe_bus_store_fence();
    } else {
        probe_cacheable_store_fence();
    }
}

static inline void probe_load_fence(probe_fence_t fence)
{
    if (fence == PROBE_FENCE_BUS) {
        probe_bus_load_fence();
    } else {
        probe_cacheable_load_fence();
    }
}

static inline void probe_spin_hint(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

static inline uint32_t probe_word_load(volatile uint32_t *ptr)
{
    return *ptr;
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

static int probe_open_posix(probe_mapping_t *mapping, size_t map_len)
{
    int fd;

    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
    snprintf(mapping->shm_name, sizeof(mapping->shm_name),
             "/obmm_probe_%ld_%ld", (long)getpid(), (long)time(NULL));
    fd = shm_open(mapping->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        return -1;
    }

    if (ftruncate(fd, (off_t)map_len) != 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(mapping->shm_name);
        return -1;
    }

    mapping->base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         0);
    if (mapping->base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(mapping->shm_name);
        return -1;
    }

    mapping->length   = map_len;
    mapping->fd       = fd;
    mapping->is_posix = 1;
    shm_unlink(mapping->shm_name);
    return 0;
}

static int probe_open_obmm(probe_mapping_t *mapping, const char *dev_path,
                           off_t offset, size_t map_len)
{
    int fd;

    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
    fd = open(dev_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    mapping->base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         offset);
    if (mapping->base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    mapping->length   = map_len;
    mapping->fd       = fd;
    mapping->is_posix = 0;
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
}

static void probe_child_loop(probe_shared_t *shared, const probe_opts_t *opts,
                             probe_fence_t fence)
{
    uint8_t            *rx_buf;
    probe_proc_stats_t *stats = &shared->child_stats;
    unsigned            total_iters = opts->warmup + opts->iters;
    unsigned            i;

    if (probe_set_affinity(opts->cpu_child) != 0) {
        perror("sched_setaffinity(child)");
        _exit(2);
    }

    rx_buf = malloc(opts->msg_size ? opts->msg_size : 1);
    if (rx_buf == NULL) {
        perror("malloc");
        _exit(2);
    }

    memset(stats, 0, sizeof(*stats));
    probe_store_fence(fence);
    probe_word_store(&shared->child_ready, 1);
    while (probe_word_load(&shared->start) == 0) {
        probe_spin_hint();
    }
    probe_load_fence(fence);

    for (i = 1; i <= total_iters; ++i) {
        uint64_t start_wait;
        uint64_t start_copy;
        uint64_t start_publish;
        uint32_t observed;
        int      measure = (i > opts->warmup);

        start_wait = probe_now_ns();
        for (;;) {
            observed = probe_word_load(&shared->to_child.seq);
            if (measure) {
                stats->spins++;
            }
            if (observed == i) {
                break;
            }
            probe_spin_hint();
        }
        if (measure) {
            stats->wait_ns += probe_now_ns() - start_wait;
        }

        probe_load_fence(fence);
        if (opts->msg_size > 0) {
            start_copy = probe_now_ns();
            memcpy(rx_buf, shared->to_child.payload, opts->msg_size);
            memcpy(shared->to_parent.payload, rx_buf, opts->msg_size);
            if (measure) {
                stats->copy_ns += probe_now_ns() - start_copy;
                stats->checksum += probe_checksum_bytes(rx_buf, opts->msg_size);
            }
        }

        start_publish = probe_now_ns();
        probe_store_fence(fence);
        probe_word_store(&shared->to_parent.seq, i);
        if (measure) {
            stats->publish_ns += probe_now_ns() - start_publish;
        }
    }
    free(rx_buf);
    _exit(0);
}

static int probe_run_once(const probe_opts_t *opts, const char *mode_name,
                          probe_fence_t fence, probe_mapping_t *mapping,
                          probe_result_t *result)
{
    probe_shared_t  *shared = (probe_shared_t*)mapping->base;
    uint8_t         *tx_buf;
    uint8_t         *rx_buf;
    unsigned         total_iters = opts->warmup + opts->iters;
    uint64_t         start_ns;
    uint64_t         end_ns;
    pid_t            pid;
    int              status;
    unsigned         i;

    if (sizeof(*shared) > mapping->length) {
        fprintf(stderr, "mapped region too small: need %zu bytes, got %zu\n",
                sizeof(*shared), mapping->length);
        return -1;
    }

    if (probe_set_affinity(opts->cpu_parent) != 0) {
        perror("sched_setaffinity(parent)");
        return -1;
    }

    memset(shared, 0, sizeof(*shared));
    tx_buf = malloc(opts->msg_size ? opts->msg_size : 1);
    rx_buf = malloc(opts->msg_size ? opts->msg_size : 1);
    if ((tx_buf == NULL) || (rx_buf == NULL)) {
        perror("malloc");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    memset(tx_buf, 0xa5, opts->msg_size);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    if (pid == 0) {
        probe_child_loop(shared, opts, fence);
    }

    while (probe_word_load(&shared->child_ready) == 0) {
        probe_spin_hint();
    }
    probe_load_fence(fence);
    probe_store_fence(fence);
    probe_word_store(&shared->start, 1);

    for (i = 1; i <= total_iters; ++i) {
        uint64_t start_copy;
        uint64_t start_publish;
        uint64_t start_wait;
        uint32_t observed;
        int      measure = (i > opts->warmup);

        if (i == (opts->warmup + 1)) {
            start_ns = probe_now_ns();
        }

        if (opts->msg_size > 0) {
            start_copy = probe_now_ns();
            memcpy(shared->to_child.payload, tx_buf, opts->msg_size);
            if (measure) {
                shared->parent_stats.copy_ns += probe_now_ns() - start_copy;
            }
        }

        start_publish = probe_now_ns();
        probe_store_fence(fence);
        probe_word_store(&shared->to_child.seq, i);
        if (measure) {
            shared->parent_stats.publish_ns += probe_now_ns() - start_publish;
        }

        start_wait = probe_now_ns();
        for (;;) {
            observed = probe_word_load(&shared->to_parent.seq);
            if (measure) {
                shared->parent_stats.spins++;
            }
            if (observed == i) {
                break;
            }
            probe_spin_hint();
        }
        if (measure) {
            shared->parent_stats.wait_ns += probe_now_ns() - start_wait;
        }

        probe_load_fence(fence);
        if (opts->msg_size > 0) {
            memcpy(rx_buf, shared->to_parent.payload, opts->msg_size);
            if (measure) {
                shared->parent_stats.checksum +=
                        probe_checksum_bytes(rx_buf, opts->msg_size);
            }
        }
    }
    end_ns = probe_now_ns();

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        fprintf(stderr, "%s child exited abnormally\n", mode_name);
        free(tx_buf);
        free(rx_buf);
        return -1;
    }

    memset(result, 0, sizeof(*result));
    result->mode_name         = mode_name;
    result->half_rtt_ns       = (double)(end_ns - start_ns) / opts->iters / 2.0;
    result->parent_wait_ns    = (double)shared->parent_stats.wait_ns / opts->iters;
    result->parent_copy_ns    = (double)shared->parent_stats.copy_ns / opts->iters;
    result->parent_publish_ns = (double)shared->parent_stats.publish_ns / opts->iters;
    result->parent_spins      = (double)shared->parent_stats.spins / opts->iters;
    result->child_wait_ns     = (double)shared->child_stats.wait_ns / opts->iters;
    result->child_copy_ns     = (double)shared->child_stats.copy_ns / opts->iters;
    result->child_publish_ns  = (double)shared->child_stats.publish_ns / opts->iters;
    result->child_spins       = (double)shared->child_stats.spins / opts->iters;
    result->parent_checksum   = shared->parent_stats.checksum;
    result->child_checksum    = shared->child_stats.checksum;

    printf("mode=%s size=%zu iters=%u warmup=%u half_rtt_ns=%.2f "
           "parent_wait_ns=%.2f parent_copy_ns=%.2f parent_publish_ns=%.2f "
           "parent_spins=%.2f child_wait_ns=%.2f child_copy_ns=%.2f "
           "child_publish_ns=%.2f child_spins=%.2f\n",
           mode_name, opts->msg_size, opts->iters, opts->warmup,
           result->half_rtt_ns, result->parent_wait_ns, result->parent_copy_ns,
           result->parent_publish_ns, result->parent_spins,
           result->child_wait_ns, result->child_copy_ns,
           result->child_publish_ns, result->child_spins);

    free(tx_buf);
    free(rx_buf);
    return 0;
}

static int probe_parse_mode(const char *mode_str, probe_mode_t *mode_p)
{
    if (!strcmp(mode_str, "posix")) {
        *mode_p = PROBE_MODE_POSIX;
    } else if (!strcmp(mode_str, "obmm")) {
        *mode_p = PROBE_MODE_OBMM;
    } else if (!strcmp(mode_str, "both")) {
        *mode_p = PROBE_MODE_BOTH;
    } else {
        return -1;
    }
    return 0;
}

static int probe_parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    static const struct option long_opts[] = {
        {"mode",       required_argument, NULL, 'm'},
        {"size",       required_argument, NULL, 's'},
        {"iters",      required_argument, NULL, 'i'},
        {"warmup",     required_argument, NULL, 'w'},
        {"cpu-parent", required_argument, NULL, 'p'},
        {"cpu-child",  required_argument, NULL, 'c'},
        {"obmm-dev",   required_argument, NULL, 'd'},
        {"obmm-offset",required_argument, NULL, 'o'},
        {"map-len",    required_argument, NULL, 'l'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    size_t value;
    int    ch;

    memset(opts, 0, sizeof(*opts));
    opts->mode        = PROBE_MODE_BOTH;
    opts->iters       = PROBE_DEFAULT_ITERS;
    opts->warmup      = PROBE_DEFAULT_WARMUP;
    opts->msg_size    = PROBE_DEFAULT_SIZE;
    opts->map_len     = PROBE_DEFAULT_MAP_LEN;
    opts->obmm_offset = (off_t)PROBE_DEFAULT_OBMM_OFFSET;
    opts->cpu_parent  = -1;
    opts->cpu_child   = -1;

    while ((ch = getopt_long(argc, argv, "m:s:i:w:p:c:d:o:l:h",
                             long_opts, NULL)) != -1) {
        switch (ch) {
        case 'm':
            if (probe_parse_mode(optarg, &opts->mode) != 0) {
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
        case 'p':
            opts->cpu_parent = atoi(optarg);
            break;
        case 'c':
            opts->cpu_child = atoi(optarg);
            break;
        case 'd':
            snprintf(opts->obmm_dev, sizeof(opts->obmm_dev), "%s", optarg);
            opts->obmm_dev_set = 1;
            break;
        case 'o':
            if (probe_parse_size(optarg, &value) != 0) {
                return -1;
            }
            opts->obmm_offset = (off_t)value;
            break;
        case 'l':
            if (probe_parse_size(optarg, &value) != 0) {
                return -1;
            }
            opts->map_len = value;
            break;
        case 'h':
        default:
            return -1;
        }
    }

    if ((opts->msg_size > PROBE_MAX_SIZE) ||
        (opts->map_len < sizeof(probe_shared_t))) {
        return -1;
    }

    if ((opts->mode != PROBE_MODE_POSIX) &&
        ((opts->obmm_offset % sysconf(_SC_PAGESIZE)) != 0)) {
        fprintf(stderr, "obmm offset must be page-aligned\n");
        return -1;
    }

    return 0;
}

static int probe_run_posix(const probe_opts_t *opts, probe_result_t *result)
{
    probe_mapping_t mapping;
    int             rc;

    if (probe_open_posix(&mapping, opts->map_len) != 0) {
        return -1;
    }

    rc = probe_run_once(opts, "posix", PROBE_FENCE_CACHEABLE, &mapping, result);
    probe_close_mapping(&mapping);
    return rc;
}

static int probe_run_obmm(const probe_opts_t *opts, probe_result_t *result)
{
    probe_mapping_t mapping;
    char            dev_path[sizeof(opts->obmm_dev)];
    int             rc;

    if (opts->obmm_dev_set) {
        snprintf(dev_path, sizeof(dev_path), "%s", opts->obmm_dev);
    } else if (probe_discover_obmm_export(dev_path, sizeof(dev_path)) != 0) {
        fprintf(stderr, "failed to auto-discover an mmap-able obmm export device\n");
        return -1;
    }

    if (probe_open_obmm(&mapping, dev_path, opts->obmm_offset,
                        opts->map_len) != 0) {
        return -1;
    }

    rc = probe_run_once(opts, "obmm", PROBE_FENCE_BUS, &mapping, result);
    probe_close_mapping(&mapping);
    return rc;
}

int main(int argc, char **argv)
{
    probe_opts_t   opts;
    probe_result_t posix_result;
    probe_result_t obmm_result;
    int            rc = 0;

    if (probe_parse_opts(argc, argv, &opts) != 0) {
        probe_usage(argv[0]);
        return 1;
    }

    if ((opts.mode == PROBE_MODE_POSIX) || (opts.mode == PROBE_MODE_BOTH)) {
        if (probe_run_posix(&opts, &posix_result) != 0) {
            return 1;
        }
    }

    if ((opts.mode == PROBE_MODE_OBMM) || (opts.mode == PROBE_MODE_BOTH)) {
        if (probe_run_obmm(&opts, &obmm_result) != 0) {
            return 1;
        }
    }

    if (opts.mode == PROBE_MODE_BOTH) {
        printf("compare posix_half_rtt_ns=%.2f obmm_half_rtt_ns=%.2f "
               "obmm_vs_posix=%.2f\n",
               posix_result.half_rtt_ns, obmm_result.half_rtt_ns,
               obmm_result.half_rtt_ns / posix_result.half_rtt_ns);
    }

    return rc;
}
