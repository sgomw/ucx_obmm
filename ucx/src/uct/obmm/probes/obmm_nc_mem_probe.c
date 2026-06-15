/*
 * Standalone target probe for local NC mmap bandwidth.
 *
 * This intentionally avoids UCX and libobmm so it can separate the local NC
 * memory wall from UCP protocol and UCT FIFO behavior.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OBMM_SYSFS_ROOT "/sys/devices/obmm"
#define OBMM_DEV_FMT    "/dev/obmm_shmdev%" PRIu64
#define OBMM_2M         (2ul * 1024ul * 1024ul)
#define OBMM_DEFAULT_OFFSET (2ull * 1024ull * 1024ull * 1024ull)

typedef enum {
    MODE_WRITE,
    MODE_READ,
    MODE_PAIR,
    MODE_HANDOFF
} probe_mode_t;

typedef struct {
    volatile uint64_t ready;
    char              pad0[64 - sizeof(uint64_t)];
    volatile uint64_t ack;
    char              pad1[64 - sizeof(uint64_t)];
} probe_ctl_t;

typedef struct {
    uint64_t     memid;
    probe_mode_t mode;
    size_t       bytes;
    double       seconds;
    uint64_t     offset;
    int          have_offset;
    size_t       stride;
} probe_opts_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --memid N --mode write|read|pair|handoff --bytes N "
            "[--seconds S] [--offset N] [--stride N]\n", prog);
}

static int parse_u64(const char *s, uint64_t *value)
{
    char               *end = NULL;
    unsigned long long  v;

    errno = 0;
    v = strtoull(s, &end, 0);
    if ((errno != 0) || (end == s) || (*end != '\0')) {
        return -1;
    }

    *value = (uint64_t)v;
    return 0;
}

static int parse_size(const char *s, size_t *value)
{
    uint64_t v;

    if (parse_u64(s, &v) != 0) {
        return -1;
    }

    *value = (size_t)v;
    return ((uint64_t)(*value) == v) ? 0 : -1;
}

static int parse_double(const char *s, double *value)
{
    char  *end = NULL;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if ((errno != 0) || (end == s) || (*end != '\0') || (v <= 0.0)) {
        return -1;
    }

    *value = v;
    return 0;
}

static int parse_mode(const char *s, probe_mode_t *mode)
{
    if (!strcmp(s, "write")) {
        *mode = MODE_WRITE;
        return 0;
    } else if (!strcmp(s, "read")) {
        *mode = MODE_READ;
        return 0;
    } else if (!strcmp(s, "pair")) {
        *mode = MODE_PAIR;
        return 0;
    } else if (!strcmp(s, "handoff")) {
        *mode = MODE_HANDOFF;
        return 0;
    }

    return -1;
}

static const char *mode_name(probe_mode_t mode)
{
    switch (mode) {
    case MODE_WRITE:
        return "write";
    case MODE_READ:
        return "read";
    case MODE_PAIR:
        return "pair";
    case MODE_HANDOFF:
        return "handoff";
    default:
        return "unknown";
    }
}

static int parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->seconds = 5.0;
    opts->mode    = MODE_PAIR;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--memid") && (i + 1 < argc)) {
            if (parse_u64(argv[++i], &opts->memid) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--mode") && (i + 1 < argc)) {
            if (parse_mode(argv[++i], &opts->mode) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--bytes") && (i + 1 < argc)) {
            if (parse_size(argv[++i], &opts->bytes) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--seconds") && (i + 1 < argc)) {
            if (parse_double(argv[++i], &opts->seconds) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--offset") && (i + 1 < argc)) {
            if (parse_u64(argv[++i], &opts->offset) != 0) {
                return -1;
            }
            opts->have_offset = 1;
        } else if (!strcmp(argv[i], "--stride") && (i + 1 < argc)) {
            if (parse_size(argv[++i], &opts->stride) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    return ((opts->memid != 0) && (opts->bytes != 0)) ? 0 : -1;
}

static uint64_t align_up_u64(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
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
        return -1;
    }

    if (fgets(buf, sizeof(buf), f) == NULL) {
        fprintf(stderr, "failed to read %s\n", path);
        fclose(f);
        return -1;
    }

    fclose(f);

    if (sscanf(buf, "%" SCNx64, &size) != 1) {
        fprintf(stderr, "failed to parse hex size from %s: %s\n", path, buf);
        return -1;
    }

    *size_p = size;
    return 0;
}

static int get_env_int(const char *name, int *value_p)
{
    const char *s = getenv(name);
    char       *end;
    long        v;

    if ((s == NULL) || (*s == '\0')) {
        return 0;
    }

    errno = 0;
    v = strtol(s, &end, 10);
    if ((errno != 0) || (end == s) || (*end != '\0') ||
        (v < 0) || (v > INT_MAX)) {
        return 0;
    }

    *value_p = (int)v;
    return 1;
}

static int get_first_env_int(const char **names, int default_value)
{
    int i, value;

    for (i = 0; names[i] != NULL; ++i) {
        if (get_env_int(names[i], &value)) {
            return value;
        }
    }

    return default_value;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
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

static void fill_pattern(uint8_t *buf, size_t len, int rank)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(rank + (int)i);
    }
}

static uint64_t sample_checksum(const uint8_t *buf, size_t len)
{
    uint64_t sum = 0;
    size_t   i;
    size_t   step = 4096;

    if (len < step) {
        step = 1;
    }

    for (i = 0; i < len; i += step) {
        sum += buf[i];
    }

    return sum;
}

int main(int argc, char **argv)
{
    static const char *rank_envs[] = {
        "OBMM_PROBE_RANK", "OMPI_COMM_WORLD_RANK", "PMIX_RANK",
        "PMI_RANK", "SLURM_PROCID", NULL
    };
    static const char *size_envs[] = {
        "OBMM_PROBE_SIZE", "OMPI_COMM_WORLD_SIZE", "PMIX_SIZE",
        "PMI_SIZE", "SLURM_NTASKS", NULL
    };
    static const char *local_rank_envs[] = {
        "OBMM_PROBE_LOCAL_RANK", "OMPI_COMM_WORLD_LOCAL_RANK",
        "MPI_LOCALRANKID", "SLURM_LOCALID", NULL
    };
    static const char *local_size_envs[] = {
        "OBMM_PROBE_LOCAL_SIZE", "OMPI_COMM_WORLD_LOCAL_SIZE",
        "MPI_LOCALNRANKS", NULL
    };

    probe_opts_t opts;
    char         dev_path[PATH_MAX];
    uint64_t     region_size;
    uint64_t     need;
    uint64_t     offset;
    uint64_t     slice_index;
    uint64_t     slice_count;
    size_t       stride;
    int          rank, size, local_rank, local_size;
    int          fd;
    uint8_t     *map;
    uint8_t     *src;
    uint8_t     *dst;
    uint8_t     *nc_ptr;
    probe_ctl_t *ctl;
    uint8_t     *payload;
    const char  *role;
    double       start, end, t0, t1;
    uint64_t     ops = 0;
    uint64_t     checksum = 0;
    uint64_t     seq;
    double       elapsed;
    double       bw_gibs;

    if (parse_opts(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }

    if (read_region_size(opts.memid, &region_size) != 0) {
        return 1;
    }

    rank       = get_first_env_int(rank_envs, 0);
    size       = get_first_env_int(size_envs, 1);
    local_rank = get_first_env_int(local_rank_envs, rank);
    local_size = get_first_env_int(local_size_envs, size);

    if ((local_rank < 0) || (local_size <= 0) || (local_rank >= local_size)) {
        fprintf(stderr, "invalid local rank/size: local_rank=%d local_size=%d\n",
                local_rank, local_size);
        return 1;
    }

    if (opts.mode == MODE_HANDOFF) {
        size_t min_stride = sizeof(probe_ctl_t) + opts.bytes;

        stride = opts.stride ? opts.stride :
                 (size_t)align_up_u64(min_stride, OBMM_2M);
        if (stride < min_stride) {
            fprintf(stderr,
                    "stride=%zu is smaller than handoff record size=%zu\n",
                    stride, min_stride);
            return 1;
        }
    } else {
        stride = opts.stride ? opts.stride :
                 (size_t)align_up_u64(opts.bytes, OBMM_2M);
        if (stride < opts.bytes) {
            fprintf(stderr, "stride=%zu is smaller than bytes=%zu\n",
                    stride, opts.bytes);
            return 1;
        }
    }

    if ((opts.mode == MODE_PAIR) || (opts.mode == MODE_HANDOFF)) {
        if ((local_size % 2) != 0) {
            fprintf(stderr, "%s mode requires even local_size, got %d\n",
                    mode_name(opts.mode), local_size);
            return 1;
        }
        slice_count = (uint64_t)local_size / 2;
        if (local_rank < (local_size / 2)) {
            slice_index = (uint64_t)local_rank;
            role        = "writer";
        } else {
            slice_index = (uint64_t)(local_rank - (local_size / 2));
            role        = "reader";
        }
    } else {
        slice_count = (uint64_t)local_size;
        slice_index = (uint64_t)local_rank;
        role        = (opts.mode == MODE_WRITE) ? "writer" : "reader";
    }

    need = slice_count * (uint64_t)stride;
    if (opts.have_offset) {
        offset = opts.offset;
    } else if (region_size > (OBMM_DEFAULT_OFFSET + need)) {
        offset = OBMM_DEFAULT_OFFSET;
    } else {
        offset = 64ull * 1024ull * 1024ull;
    }

    if ((offset > region_size) || (need > (region_size - offset))) {
        fprintf(stderr,
                "region too small: size=%" PRIu64 " offset=%" PRIu64
                " need=%" PRIu64 " local_size=%d stride=%zu\n",
                region_size, offset, need, local_size, stride);
        return 1;
    }

    snprintf(dev_path, sizeof(dev_path), OBMM_DEV_FMT, opts.memid);
    fd = open(dev_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev_path, strerror(errno));
        return 1;
    }

    map = mmap(NULL, (size_t)region_size, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, size=%" PRIu64 ") failed: %s\n",
                dev_path, region_size, strerror(errno));
        close(fd);
        return 1;
    }

    if (posix_memalign((void**)&src, 64, opts.bytes) != 0) {
        fprintf(stderr, "posix_memalign(src, %zu) failed\n", opts.bytes);
        munmap(map, (size_t)region_size);
        close(fd);
        return 1;
    }
    if (posix_memalign((void**)&dst, 64, opts.bytes) != 0) {
        fprintf(stderr, "posix_memalign(dst, %zu) failed\n", opts.bytes);
        free(src);
        munmap(map, (size_t)region_size);
        close(fd);
        return 1;
    }

    fill_pattern(src, opts.bytes, rank);
    memset(dst, 0, opts.bytes);

    nc_ptr  = map + offset + (slice_index * stride);
    ctl     = (probe_ctl_t*)nc_ptr;
    payload = nc_ptr;
    if (opts.mode == MODE_HANDOFF) {
        payload = nc_ptr + sizeof(*ctl);
        if (!strcmp(role, "writer")) {
            ctl->ready = 0;
            ctl->ack   = 0;
            nc_full_barrier();
        }
    }

    if ((opts.mode != MODE_HANDOFF) || !strcmp(role, "writer")) {
        memcpy(payload, src, opts.bytes);
    }

    start = now_sec() + 2.0;
    while (now_sec() < start) {
        /* spin to align independently launched ranks */
    }
    end = start + opts.seconds;

    t0 = now_sec();
    if (opts.mode == MODE_HANDOFF) {
        if (!strcmp(role, "writer")) {
            seq = 1;
            while (now_sec() < end) {
                while (ctl->ack != (seq - 1)) {
                    if (now_sec() >= end) {
                        goto out_timed;
                    }
                }
                memcpy(payload, src, opts.bytes);
                nc_full_barrier();
                ctl->ready = seq;
                ++ops;
                ++seq;
            }
        } else {
            seq = 1;
            while (now_sec() < end) {
                while (ctl->ready != seq) {
                    if (now_sec() >= end) {
                        goto out_timed;
                    }
                }
                nc_full_barrier();
                memcpy(dst, payload, opts.bytes);
                nc_full_barrier();
                ctl->ack = seq;
                ++ops;
                ++seq;
            }
        }
out_timed:
        checksum = !strcmp(role, "writer") ?
                   sample_checksum(src, opts.bytes) :
                   sample_checksum(dst, opts.bytes);
    } else if ((opts.mode == MODE_WRITE) ||
        ((opts.mode == MODE_PAIR) && !strcmp(role, "writer"))) {
        while (now_sec() < end) {
            memcpy(payload, src, opts.bytes);
            ++ops;
        }
        checksum = sample_checksum(src, opts.bytes);
    } else {
        while (now_sec() < end) {
            memcpy(dst, payload, opts.bytes);
            ++ops;
        }
        checksum = sample_checksum(dst, opts.bytes);
    }
    t1 = now_sec();

    elapsed = t1 - t0;
    bw_gibs = ((double)ops * (double)opts.bytes) /
              (elapsed * 1024.0 * 1024.0 * 1024.0);

    printf("OBMM_NC_MEM_PROBE rank=%d size=%d local_rank=%d local_size=%d "
           "memid=%" PRIu64 " mode=%s role=%s bytes=%zu stride=%zu "
           "offset=%" PRIu64 " ops=%" PRIu64 " seconds=%.6f "
           "bw_GiBs=%.6f checksum=%" PRIu64 "\n",
           rank, size, local_rank, local_size, opts.memid,
           mode_name(opts.mode), role, opts.bytes, stride, offset, ops,
           elapsed, bw_gibs, checksum);

    free(dst);
    free(src);
    munmap(map, (size_t)region_size);
    close(fd);
    return 0;
}
