/*
 * Standalone target probe for mapping one OBMM export twice: cacheable and
 * non-cacheable. It intentionally avoids UCX and libobmm.
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

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

#define OBMM_SYSFS_ROOT       "/sys/devices/obmm"
#define OBMM_DEV_FMT          "/dev/obmm_shmdev%" PRIu64
#define OBMM_CACHELINE        64ul
#define OBMM_2M               (2ul * 1024ul * 1024ul)
#define OBMM_DEFAULT_BYTES    4096ul
#define OBMM_DEFAULT_ITERS    10000ull
#define OBMM_DEFAULT_TIMEOUT  5.0
#define OBMM_DEFAULT_DELAY    2.0

typedef enum {
    MODE_MAP_ONLY,
    MODE_ALIAS,
    MODE_CC_HANDOFF,
    MODE_NC_HANDOFF,
    MODE_MIXED_CTL,
    MODE_SPLIT_CTL
} probe_mode_t;

typedef enum {
    MAP_CC,
    MAP_NC
} map_kind_t;

typedef enum {
    MAP_ORDER_CC_FIRST,
    MAP_ORDER_NC_FIRST
} map_order_t;

typedef struct {
    volatile uint64_t ready;
    char              pad0[OBMM_CACHELINE - sizeof(uint64_t)];
    volatile uint64_t ack;
    char              pad1[OBMM_CACHELINE - sizeof(uint64_t)];
} probe_ctl_t;

typedef struct {
    uint64_t     memid;
    probe_mode_t mode;
    size_t       bytes;
    uint64_t     iters;
    uint64_t     offset;
    int          have_offset;
    size_t       stride;
    double       timeout;
    double       start_delay;
    map_order_t  map_order;
    map_kind_t   first_map;
    map_kind_t   second_map;
    int          full_map;
} probe_opts_t;

typedef struct {
    uint8_t *cc;
    uint8_t *nc;
    uint64_t region_size;
    uint64_t map_offset;
    uint64_t map_length;
} probe_maps_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --memid N [--mode alias|cc-handoff|nc-handoff|"
            "mixed-ctl|split-ctl|map-only]\n"
            "       [--bytes N] [--iters N] [--offset N] [--stride N]\n"
            "       [--timeout S] [--start-delay S]\n"
            "       [--map-order cc-first|nc-first]\n"
            "       [--map-pair cc-nc|nc-cc|cc-cc|nc-nc] [--full-map]\n",
            prog);
}

static int parse_u64_base(const char *s, int base, uint64_t *value)
{
    char               *end = NULL;
    unsigned long long  v;

    errno = 0;
    v = strtoull(s, &end, base);
    if ((errno != 0) || (end == s) || (*end != '\0')) {
        return -1;
    }

    *value = (uint64_t)v;
    return 0;
}

static int parse_u64(const char *s, uint64_t *value)
{
    return parse_u64_base(s, 0, value);
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
    if (!strcmp(s, "map-only")) {
        *mode = MODE_MAP_ONLY;
        return 0;
    } else if (!strcmp(s, "alias")) {
        *mode = MODE_ALIAS;
        return 0;
    } else if (!strcmp(s, "cc-handoff")) {
        *mode = MODE_CC_HANDOFF;
        return 0;
    } else if (!strcmp(s, "nc-handoff")) {
        *mode = MODE_NC_HANDOFF;
        return 0;
    } else if (!strcmp(s, "mixed-ctl")) {
        *mode = MODE_MIXED_CTL;
        return 0;
    } else if (!strcmp(s, "split-ctl")) {
        *mode = MODE_SPLIT_CTL;
        return 0;
    }

    return -1;
}

static const char *mode_name(probe_mode_t mode)
{
    switch (mode) {
    case MODE_MAP_ONLY:
        return "map-only";
    case MODE_ALIAS:
        return "alias";
    case MODE_CC_HANDOFF:
        return "cc-handoff";
    case MODE_NC_HANDOFF:
        return "nc-handoff";
    case MODE_MIXED_CTL:
        return "mixed-ctl";
    case MODE_SPLIT_CTL:
        return "split-ctl";
    default:
        return "unknown";
    }
}

static const char *map_name(map_kind_t kind)
{
    return (kind == MAP_CC) ? "cc" : "nc";
}

static const char *map_order_name(map_order_t order)
{
    return (order == MAP_ORDER_CC_FIRST) ? "cc-first" : "nc-first";
}

static const char *map_pair_name(const probe_opts_t *opts)
{
    if ((opts->first_map == MAP_CC) && (opts->second_map == MAP_NC)) {
        return "cc-nc";
    } else if ((opts->first_map == MAP_NC) && (opts->second_map == MAP_CC)) {
        return "nc-cc";
    } else if ((opts->first_map == MAP_CC) && (opts->second_map == MAP_CC)) {
        return "cc-cc";
    }

    return "nc-nc";
}

static int parse_map_order(const char *s, map_order_t *order)
{
    if (!strcmp(s, "cc-first")) {
        *order = MAP_ORDER_CC_FIRST;
        return 0;
    } else if (!strcmp(s, "nc-first")) {
        *order = MAP_ORDER_NC_FIRST;
        return 0;
    }

    return -1;
}

static int parse_map_pair(const char *s, map_kind_t *first, map_kind_t *second)
{
    if (!strcmp(s, "cc-nc")) {
        *first  = MAP_CC;
        *second = MAP_NC;
        return 0;
    } else if (!strcmp(s, "nc-cc")) {
        *first  = MAP_NC;
        *second = MAP_CC;
        return 0;
    } else if (!strcmp(s, "cc-cc")) {
        *first  = MAP_CC;
        *second = MAP_CC;
        return 0;
    } else if (!strcmp(s, "nc-nc")) {
        *first  = MAP_NC;
        *second = MAP_NC;
        return 0;
    }

    return -1;
}

static int parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->mode        = MODE_ALIAS;
    opts->bytes       = OBMM_DEFAULT_BYTES;
    opts->iters       = OBMM_DEFAULT_ITERS;
    opts->timeout     = OBMM_DEFAULT_TIMEOUT;
    opts->start_delay = OBMM_DEFAULT_DELAY;
    opts->map_order   = MAP_ORDER_CC_FIRST;
    opts->first_map   = MAP_CC;
    opts->second_map  = MAP_NC;

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
        } else if (!strcmp(argv[i], "--iters") && (i + 1 < argc)) {
            if (parse_u64(argv[++i], &opts->iters) != 0) {
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
        } else if (!strcmp(argv[i], "--timeout") && (i + 1 < argc)) {
            if (parse_double(argv[++i], &opts->timeout) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--start-delay") && (i + 1 < argc)) {
            if (parse_double(argv[++i], &opts->start_delay) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--map-order") && (i + 1 < argc)) {
            if (parse_map_order(argv[++i], &opts->map_order) != 0) {
                return -1;
            }
            if (opts->map_order == MAP_ORDER_CC_FIRST) {
                opts->first_map  = MAP_CC;
                opts->second_map = MAP_NC;
            } else {
                opts->first_map  = MAP_NC;
                opts->second_map = MAP_CC;
            }
        } else if (!strcmp(argv[i], "--map-pair") && (i + 1 < argc)) {
            if (parse_map_pair(argv[++i], &opts->first_map,
                               &opts->second_map) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--full-map")) {
            opts->full_map = 1;
        } else {
            return -1;
        }
    }

    return ((opts->memid != 0) && (opts->bytes != 0) &&
            (opts->iters != 0)) ? 0 : -1;
}

static int map_pair_has_cc_and_nc(const probe_opts_t *opts)
{
    return (opts->first_map != opts->second_map);
}

static uint64_t align_up_u64(uint64_t value, uint64_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static uint64_t align_down_u64(uint64_t value, uint64_t align)
{
    return value & ~(align - 1);
}

static int parse_sysfs_u64(const char *s, uint64_t *value)
{
    const char *p;
    int         base = 0;

    for (p = s; *p != '\0'; ++p) {
        if (((*p >= 'a') && (*p <= 'f')) || ((*p >= 'A') && (*p <= 'F'))) {
            base = 16;
            break;
        }
    }

    return parse_u64_base(s, base, value);
}

static int read_region_size(uint64_t memid, uint64_t *size_p)
{
    char     path[PATH_MAX];
    char     buf[64];
    FILE    *f;
    char    *nl;

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

    nl = strpbrk(buf, " \t\r\n");
    if (nl != NULL) {
        *nl = '\0';
    }

    if (parse_sysfs_u64(buf, size_p) != 0) {
        fprintf(stderr, "failed to parse size from %s: %s\n", path, buf);
        return -1;
    }

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

static void cpu_full_fence(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb ish" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("mfence" ::: "memory");
#elif defined(__powerpc64__)
    __asm__ __volatile__("sync" ::: "memory");
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ __volatile__("fence rw, rw" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static void nc_full_fence(void)
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

static void map_fence(map_kind_t kind)
{
    if (kind == MAP_CC) {
        cpu_full_fence();
    } else {
        nc_full_fence();
    }
}

static size_t record_size(size_t payload_bytes)
{
    return (size_t)align_up_u64(sizeof(probe_ctl_t) + payload_bytes,
                                OBMM_CACHELINE);
}

static probe_ctl_t *record_ctl(uint8_t *base)
{
    return (probe_ctl_t*)base;
}

static uint8_t *record_payload(uint8_t *base)
{
    return base + sizeof(probe_ctl_t);
}

static void fill_pattern(uint8_t *buf, size_t len, uint64_t seq, uint64_t salt)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(seq * 131u + salt * 17u + (uint64_t)i * 31u);
    }
}

static int verify_pattern(const uint8_t *buf, size_t len, uint64_t seq,
                          uint64_t salt, size_t *bad_index_p)
{
    size_t  i;
    uint8_t expected;

    for (i = 0; i < len; ++i) {
        expected = (uint8_t)(seq * 131u + salt * 17u + (uint64_t)i * 31u);
        if (buf[i] != expected) {
            *bad_index_p = i;
            return -1;
        }
    }

    return 0;
}

static uint64_t sample_checksum(const uint8_t *buf, size_t len)
{
    uint64_t sum = 0;
    size_t   step = (len < 4096) ? 1 : 4096;
    size_t   i;

    for (i = 0; i < len; i += step) {
        sum = (sum << 5) - sum + buf[i];
    }

    return sum;
}

static int wait_u64(volatile uint64_t *ptr, uint64_t target, double timeout,
                    uint64_t *last_value_p)
{
    double deadline = now_sec() + timeout;
    uint64_t value;

    do {
        value = *ptr;
        if (value == target) {
            return 0;
        }
    } while (now_sec() < deadline);

    *last_value_p = value;
    return -1;
}

static int open_one_map(uint64_t memid, uint64_t map_offset,
                        uint64_t map_length, map_kind_t kind,
                        uint8_t **map_p, int *fd_p)
{
    char dev_path[PATH_MAX];
    int  flags = O_RDWR | O_CLOEXEC;
    int  fd;
    void *map;

    if (kind == MAP_NC) {
        flags |= O_SYNC;
    }

    snprintf(dev_path, sizeof(dev_path), OBMM_DEV_FMT, memid);
    fd = open(dev_path, flags);
    if (fd < 0) {
        fprintf(stderr, "open(%s, %s) failed: %s\n", dev_path,
                map_name(kind), strerror(errno));
        return -1;
    }

    map = mmap(NULL, (size_t)map_length, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, (off_t)map_offset);
    if (map == MAP_FAILED) {
        fprintf(stderr,
                "mmap(%s, %s, offset=%" PRIu64 " size=%" PRIu64
                ") failed: %s\n",
                dev_path, map_name(kind), map_offset, map_length,
                strerror(errno));
        close(fd);
        return -1;
    }

    *map_p = map;
    *fd_p  = fd;
    return 0;
}

static uint8_t **map_slot_for_kind(probe_maps_t *maps, map_kind_t kind)
{
    if ((kind == MAP_CC) && (maps->cc == NULL)) {
        return &maps->cc;
    }

    if ((kind == MAP_NC) && (maps->nc == NULL)) {
        return &maps->nc;
    }

    return (maps->cc == NULL) ? &maps->cc : &maps->nc;
}

static int *fd_slot_for_kind(map_kind_t kind, int *fd_cc_p, int *fd_nc_p)
{
    if ((kind == MAP_CC) && (*fd_cc_p < 0)) {
        return fd_cc_p;
    }

    if ((kind == MAP_NC) && (*fd_nc_p < 0)) {
        return fd_nc_p;
    }

    return (*fd_cc_p < 0) ? fd_cc_p : fd_nc_p;
}

static void close_maps(probe_maps_t *maps, int *fd_cc_p, int *fd_nc_p)
{
    if (maps->cc != NULL) {
        munmap(maps->cc, (size_t)maps->map_length);
        maps->cc = NULL;
    }

    if (maps->nc != NULL) {
        munmap(maps->nc, (size_t)maps->map_length);
        maps->nc = NULL;
    }

    if (*fd_cc_p >= 0) {
        close(*fd_cc_p);
        *fd_cc_p = -1;
    }

    if (*fd_nc_p >= 0) {
        close(*fd_nc_p);
        *fd_nc_p = -1;
    }
}

static int open_maps(const probe_opts_t *opts, probe_maps_t *maps,
                     int *fd_cc_p, int *fd_nc_p)
{
    int status;
    uint8_t **first_map_p;
    uint8_t **second_map_p;
    int      *first_fd_p;
    int      *second_fd_p;

    first_map_p = map_slot_for_kind(maps, opts->first_map);
    first_fd_p  = fd_slot_for_kind(opts->first_map, fd_cc_p, fd_nc_p);

    status = open_one_map(opts->memid, maps->map_offset, maps->map_length,
                          opts->first_map, first_map_p, first_fd_p);
    if (status != 0) {
        return status;
    }

    second_map_p = map_slot_for_kind(maps, opts->second_map);
    second_fd_p  = fd_slot_for_kind(opts->second_map, fd_cc_p, fd_nc_p);

    status = open_one_map(opts->memid, maps->map_offset, maps->map_length,
                          opts->second_map, second_map_p, second_fd_p);
    if (status != 0) {
        close_maps(maps, fd_cc_p, fd_nc_p);
    }
    return status;
}

static uint8_t *select_record(const probe_maps_t *maps, uint64_t base_offset,
                              size_t rec_size, uint64_t pair_index,
                              size_t stride, probe_mode_t mode,
                              map_kind_t kind)
{
    uint64_t offset = base_offset + (pair_index * (uint64_t)stride);

    if ((mode == MODE_SPLIT_CTL) && (kind == MAP_NC)) {
        offset += rec_size;
    }

    return ((kind == MAP_CC) ? maps->cc : maps->nc) + offset;
}

static map_kind_t handoff_kind(probe_mode_t mode, uint64_t seq)
{
    switch (mode) {
    case MODE_CC_HANDOFF:
        return MAP_CC;
    case MODE_NC_HANDOFF:
        return MAP_NC;
    case MODE_MIXED_CTL:
    case MODE_SPLIT_CTL:
        return (seq & 1) ? MAP_CC : MAP_NC;
    default:
        return MAP_CC;
    }
}

static int run_alias_mode(const probe_opts_t *opts, const probe_maps_t *maps,
                          uint64_t offset, uint8_t *src, uint8_t *dst)
{
    uint64_t cc_to_nc_errors = 0;
    uint64_t nc_to_cc_errors = 0;
    uint64_t first_error_seq = 0;
    size_t   first_bad_index = 0;
    uint8_t *cc_payload = maps->cc + offset + sizeof(probe_ctl_t);
    uint8_t *nc_payload = maps->nc + offset + sizeof(probe_ctl_t);
    uint64_t seq;

    for (seq = 1; seq <= opts->iters; ++seq) {
        fill_pattern(src, opts->bytes, seq, 1);
        memcpy(cc_payload, src, opts->bytes);
        map_fence(MAP_CC);
        memcpy(dst, nc_payload, opts->bytes);
        map_fence(MAP_NC);
        if (verify_pattern(dst, opts->bytes, seq, 1, &first_bad_index) != 0) {
            ++cc_to_nc_errors;
            if (first_error_seq == 0) {
                first_error_seq = seq;
            }
        }

        fill_pattern(src, opts->bytes, seq, 2);
        memcpy(nc_payload, src, opts->bytes);
        map_fence(MAP_NC);
        memcpy(dst, cc_payload, opts->bytes);
        map_fence(MAP_CC);
        if (verify_pattern(dst, opts->bytes, seq, 2, &first_bad_index) != 0) {
            ++nc_to_cc_errors;
            if (first_error_seq == 0) {
                first_error_seq = seq;
            }
        }
    }

    printf("OBMM_ALIAS_PROBE mode=%s memid=%" PRIu64
           " bytes=%zu iters=%" PRIu64 " offset=%" PRIu64
           " map_offset=%" PRIu64 " map_length=%" PRIu64
           " cc_to_nc_errors=%" PRIu64 " nc_to_cc_errors=%" PRIu64
           " first_error_seq=%" PRIu64 " first_bad_index=%zu"
           " checksum=%" PRIu64 " status=%s\n",
           mode_name(opts->mode), opts->memid, opts->bytes, opts->iters,
           maps->map_offset + offset, maps->map_offset, maps->map_length,
           cc_to_nc_errors, nc_to_cc_errors, first_error_seq, first_bad_index,
           sample_checksum(dst, opts->bytes),
           ((cc_to_nc_errors == 0) && (nc_to_cc_errors == 0)) ? "PASS" :
                                                                "FAIL");

    return ((cc_to_nc_errors == 0) && (nc_to_cc_errors == 0)) ? 0 : 1;
}

static int run_handoff_mode(const probe_opts_t *opts, const probe_maps_t *maps,
                            uint64_t offset, size_t stride, size_t rec_size,
                            int rank, int size, int local_rank,
                            int local_size, uint8_t *src, uint8_t *dst)
{
    uint64_t pair_index;
    uint64_t errors = 0;
    uint64_t timeouts = 0;
    uint64_t first_error_seq = 0;
    uint64_t last_value = 0;
    uint64_t seq;
    size_t   bad_index = 0;
    double   start;
    int      is_writer;
    int      status;

    if ((local_size <= 1) || ((local_size % 2) != 0)) {
        fprintf(stderr, "%s mode requires an even local_size, got %d\n",
                mode_name(opts->mode), local_size);
        return 1;
    }

    is_writer = local_rank < (local_size / 2);
    pair_index = is_writer ? (uint64_t)local_rank :
                             (uint64_t)(local_rank - (local_size / 2));

    if (is_writer) {
        uint64_t clear_bytes = (opts->mode == MODE_SPLIT_CTL) ?
                               (uint64_t)rec_size * 2u : (uint64_t)rec_size;
        uint8_t *clear_base  = maps->nc + offset + (pair_index * stride);

        memset(clear_base, 0, (size_t)clear_bytes);
        map_fence(MAP_NC);
    }

    start = now_sec() + opts->start_delay;
    while (now_sec() < start) {
        /* give peer ranks time to map and observe initialization */
    }

    status = 0;
    for (seq = 1; seq <= opts->iters; ++seq) {
        map_kind_t   kind = handoff_kind(opts->mode, seq);
        uint8_t     *rec  = select_record(maps, offset, rec_size, pair_index,
                                          stride, opts->mode, kind);
        probe_ctl_t *ctl  = record_ctl(rec);
        uint8_t     *payload = record_payload(rec);

        if (is_writer) {
            fill_pattern(src, opts->bytes, seq, pair_index + 1);
            memcpy(payload, src, opts->bytes);
            map_fence(kind);
            ctl->ready = seq;
            map_fence(kind);

            if (wait_u64(&ctl->ack, seq, opts->timeout, &last_value) != 0) {
                ++timeouts;
                if (first_error_seq == 0) {
                    first_error_seq = seq;
                }
                status = 1;
                break;
            }
        } else {
            if (wait_u64(&ctl->ready, seq, opts->timeout, &last_value) != 0) {
                ++timeouts;
                if (first_error_seq == 0) {
                    first_error_seq = seq;
                }
                status = 1;
                break;
            }

            map_fence(kind);
            memcpy(dst, payload, opts->bytes);
            map_fence(kind);

            if (verify_pattern(dst, opts->bytes, seq, pair_index + 1,
                               &bad_index) != 0) {
                ++errors;
                if (first_error_seq == 0) {
                    first_error_seq = seq;
                }
                status = 1;
                break;
            }

            ctl->ack = seq;
            map_fence(kind);
        }
    }

    printf("OBMM_ALIAS_PROBE rank=%d size=%d local_rank=%d local_size=%d "
           "mode=%s role=%s pair=%" PRIu64 " memid=%" PRIu64
           " bytes=%zu iters=%" PRIu64 " completed=%" PRIu64
           " offset=%" PRIu64 " map_offset=%" PRIu64
           " map_length=%" PRIu64 " stride=%zu record_size=%zu"
           " errors=%" PRIu64 " timeouts=%" PRIu64
           " first_error_seq=%" PRIu64 " bad_index=%zu"
           " last_value=%" PRIu64 " checksum=%" PRIu64 " status=%s\n",
           rank, size, local_rank, local_size, mode_name(opts->mode),
           is_writer ? "writer" : "reader", pair_index, opts->memid,
           opts->bytes, opts->iters, seq - 1, maps->map_offset + offset,
           maps->map_offset, maps->map_length, stride, rec_size, errors,
           timeouts, first_error_seq, bad_index, last_value,
           sample_checksum(is_writer ? src : dst, opts->bytes), status ?
           "FAIL" : "PASS");

    return status;
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
    probe_maps_t maps;
    uint64_t records_per_pair;
    uint64_t pair_count;
    uint64_t need;
    uint64_t offset;
    uint64_t map_offset;
    uint64_t map_length;
    uint64_t page_size;
    long     page_size_l;
    size_t   rec_size;
    size_t   min_stride;
    size_t   stride;
    int      rank, size, local_rank, local_size;
    int      fd_cc = -1, fd_nc = -1;
    uint8_t *src = NULL;
    uint8_t *dst = NULL;
    int      ret;

    if (parse_opts(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }

    if ((opts.mode != MODE_MAP_ONLY) && !map_pair_has_cc_and_nc(&opts)) {
        fprintf(stderr, "--map-pair=%s is only valid with --mode map-only\n",
                map_pair_name(&opts));
        return 2;
    }

    memset(&maps, 0, sizeof(maps));
    if (read_region_size(opts.memid, &maps.region_size) != 0) {
        return 1;
    }
    if (maps.region_size > SIZE_MAX) {
        fprintf(stderr, "region size %" PRIu64 " does not fit in size_t\n",
                maps.region_size);
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

    rec_size = record_size(opts.bytes);
    records_per_pair = (opts.mode == MODE_SPLIT_CTL) ? 2u : 1u;
    min_stride = rec_size * (size_t)records_per_pair;
    stride = opts.stride ? opts.stride :
             (size_t)align_up_u64(min_stride, OBMM_2M);
    if (stride < min_stride) {
        fprintf(stderr, "stride=%zu is smaller than required=%zu\n",
                stride, min_stride);
        return 1;
    }

    pair_count = (opts.mode == MODE_ALIAS) ? 1u : (uint64_t)local_size / 2u;
    need = pair_count * (uint64_t)stride;

    if (opts.have_offset) {
        offset = opts.offset;
    } else {
        offset = 0;
    }

    if ((offset > maps.region_size) || (need > (maps.region_size - offset))) {
        fprintf(stderr,
                "region too small: size=%" PRIu64 " offset=%" PRIu64
                " need=%" PRIu64 " mode=%s local_size=%d stride=%zu\n",
                maps.region_size, offset, need, mode_name(opts.mode),
                local_size, stride);
        return 1;
    }

    page_size_l = sysconf(_SC_PAGESIZE);
    page_size   = (page_size_l > 0) ? (uint64_t)page_size_l : 4096;

    if (opts.full_map) {
        map_offset = 0;
        map_length = maps.region_size;
    } else {
        map_offset = align_down_u64(offset, page_size);
        map_length = align_up_u64((offset - map_offset) + need, page_size);
    }

    if ((map_offset > maps.region_size) ||
        (map_length > (maps.region_size - map_offset))) {
        fprintf(stderr,
                "map window out of range: region=%" PRIu64
                " map_offset=%" PRIu64 " map_length=%" PRIu64 "\n",
                maps.region_size, map_offset, map_length);
        return 1;
    }

    maps.map_offset = map_offset;
    maps.map_length = map_length;
    offset -= map_offset;

    if (open_maps(&opts, &maps, &fd_cc, &fd_nc) != 0) {
        return 1;
    }

    printf("OBMM_ALIAS_MAP memid=%" PRIu64 " order=%s pair=%s full_map=%d "
           "region_size=%" PRIu64 " map_offset=%" PRIu64
           " map_length=%" PRIu64 "\n",
           opts.memid, map_order_name(opts.map_order), map_pair_name(&opts),
           opts.full_map, maps.region_size, maps.map_offset, maps.map_length);

    if (opts.mode == MODE_MAP_ONLY) {
        printf("OBMM_ALIAS_PROBE mode=map-only memid=%" PRIu64
               " pair=%s map_offset=%" PRIu64 " map_length=%" PRIu64
               " status=PASS\n",
               opts.memid, map_pair_name(&opts), maps.map_offset,
               maps.map_length);
        ret = 0;
        goto out;
    }

    if (posix_memalign((void**)&src, OBMM_CACHELINE, opts.bytes) != 0) {
        fprintf(stderr, "posix_memalign(src, %zu) failed\n", opts.bytes);
        ret = 1;
        goto out;
    }
    if (posix_memalign((void**)&dst, OBMM_CACHELINE, opts.bytes) != 0) {
        fprintf(stderr, "posix_memalign(dst, %zu) failed\n", opts.bytes);
        ret = 1;
        goto out;
    }

    memset(src, 0, opts.bytes);
    memset(dst, 0, opts.bytes);

    if (opts.mode == MODE_ALIAS) {
        ret = run_alias_mode(&opts, &maps, offset, src, dst);
    } else {
        ret = run_handoff_mode(&opts, &maps, offset, stride, rec_size, rank,
                               size, local_rank, local_size, src, dst);
    }

out:
    free(dst);
    free(src);
    close_maps(&maps, &fd_cc, &fd_nc);
    return ret;
}
