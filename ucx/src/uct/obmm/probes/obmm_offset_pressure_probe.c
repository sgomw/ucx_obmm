/*
 * Standalone target probe for OBMM same-offset pressure across many shmdevs.
 *
 * This intentionally avoids UCX and libobmm. It maps one OBMM shmdev per
 * local rank and repeatedly touches a chosen block-relative offset, so the
 * same-offset and colored-offset cases differ only in address placement.
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OBMM_SYSFS_ROOT          "/sys/devices/obmm"
#define OBMM_DEV_FMT             "/dev/obmm_shmdev%" PRIu64
#define OBMM_CACHELINE           64ull
#define OBMM_2M                  (2ull * 1024ull * 1024ull)
#define OBMM_DEFAULT_OFFSET      64ull
#define OBMM_DEFAULT_COLOR_STEP  32896ull
#define OBMM_DEFAULT_SECONDS     5.0
#define OBMM_DEFAULT_START_DELAY 2.0
#define OBMM_DEFAULT_TIMEOUT     5.0
#define OBMM_DEFAULT_FIRST_MEMID 71ull
#define OBMM_DEFAULT_MEMID_COUNT 70u
#define OBMM_MAX_MEMIDS          4096u
#define OBMM_DONE_VALUE          UINT64_MAX
#define OBMM_SUMMARY_MAGIC       0x4f50534du
#define OBMM_SUMMARY_VERSION     1u
#define OBMM_SUMMARY_WAIT_SEC    30.0

typedef enum {
    MODE_LOAD,
    MODE_STORE,
    MODE_CAS,
    MODE_HANDOFF
} probe_mode_t;

typedef enum {
    LAYOUT_SAME,
    LAYOUT_COLORED
} probe_layout_t;

typedef enum {
    BACKEND_OBMM_NC,
    BACKEND_OBMM_CC,
    BACKEND_ANON
} probe_backend_t;

typedef struct {
    uint64_t values[OBMM_MAX_MEMIDS];
    size_t   count;
} memid_list_t;

typedef struct {
    memid_list_t  memids;
    probe_mode_t  mode;
    probe_layout_t layout;
    probe_backend_t backend;
    size_t        colors;
    int           have_colors;
    uint64_t      offset;
    uint64_t      color_step;
    size_t        bytes;
    double        seconds;
    double        start_delay;
    double        timeout;
    int           fence_each;
    int           allow_shared_memid;
    int           per_rank;
    int           have_summary_file;
    char          summary_file[PATH_MAX];
} probe_opts_t;

typedef struct {
    uint8_t *base;
    uint64_t map_offset;
    uint64_t map_length;
} probe_map_t;

typedef struct {
    volatile uint64_t ready;
    char              pad0[OBMM_CACHELINE - sizeof(uint64_t)];
    volatile uint64_t ack;
    char              pad1[OBMM_CACHELINE - sizeof(uint64_t)];
    uint8_t           payload[];
} handoff_record_t;

typedef struct {
    volatile uint32_t state;
    uint32_t          status;
    int32_t           rank;
    int32_t           local_rank;
    int32_t           role;
    uint32_t          reserved;
    uint64_t          memid;
    uint64_t          offset;
    uint64_t          ops;
    uint64_t          aux;
    uint64_t          checksum;
    uint64_t          memid_index;
    uint64_t          color_count;
    uint64_t          color_index;
    double            elapsed;
    double            ops_per_sec;
    double            ns_per_op;
} summary_slot_t;

typedef struct {
    uint32_t          magic;
    uint32_t          version;
    uint32_t          local_size;
    uint32_t          header_size;
    volatile uint32_t ready;
    uint32_t          reserved[11];
} summary_hdr_t;

typedef struct {
    int             active;
    char            path[PATH_MAX];
    size_t          length;
    summary_hdr_t  *hdr;
    summary_slot_t *slots;
} summary_map_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --mode load|store|cas|handoff [options]\n"
            "Options:\n"
            "  --memids LIST             comma/space separated memids/ranges\n"
            "  --memid-file FILE         file with memids\n"
            "  --backend obmm|obmm-nc|obmm-cc|anon\n"
            "  --layout same|colored     same offset or color by memid index\n"
            "  --colors N                repeat only N offset colors\n"
            "  --offset N                base offset, default 64\n"
            "  --color-step N            offset step, default 32896\n"
            "  --bytes N                 payload bytes for handoff, default 8\n"
            "  --seconds S               measurement window, default 5\n"
            "  --start-delay S           rank alignment delay, default 2\n"
            "  --timeout S               handoff peer timeout, default 5\n"
            "  --fence-each              add a bus fence after each store/load\n"
            "  --allow-shared-memid      allow multiple ranks per memid\n"
            "  --per-rank                print one line per rank\n"
            "  --summary-file FILE       local mmap file for rank summary\n"
            "For OBMM backends, default memids are 71..140 unless --memids "
            "or --memid-file is provided. Anonymous NC is not supported by "
            "standard Linux mmap.\n",
            prog);
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
    if (!strcmp(s, "load")) {
        *mode = MODE_LOAD;
        return 0;
    } else if (!strcmp(s, "store")) {
        *mode = MODE_STORE;
        return 0;
    } else if (!strcmp(s, "cas")) {
        *mode = MODE_CAS;
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
    case MODE_LOAD:
        return "load";
    case MODE_STORE:
        return "store";
    case MODE_CAS:
        return "cas";
    case MODE_HANDOFF:
        return "handoff";
    default:
        return "unknown";
    }
}

static int parse_layout(const char *s, probe_layout_t *layout)
{
    if (!strcmp(s, "same")) {
        *layout = LAYOUT_SAME;
        return 0;
    } else if (!strcmp(s, "colored")) {
        *layout = LAYOUT_COLORED;
        return 0;
    }

    return -1;
}

static const char *layout_name(probe_layout_t layout)
{
    return (layout == LAYOUT_SAME) ? "same" : "colored";
}

static int parse_backend(const char *s, probe_backend_t *backend)
{
    if (!strcmp(s, "obmm") || !strcmp(s, "obmm-nc")) {
        *backend = BACKEND_OBMM_NC;
        return 0;
    } else if (!strcmp(s, "obmm-cc")) {
        *backend = BACKEND_OBMM_CC;
        return 0;
    } else if (!strcmp(s, "anon")) {
        *backend = BACKEND_ANON;
        return 0;
    }

    return -1;
}

static const char *backend_name(probe_backend_t backend)
{
    switch (backend) {
    case BACKEND_OBMM_NC:
        return "obmm-nc";
    case BACKEND_OBMM_CC:
        return "obmm-cc";
    case BACKEND_ANON:
        return "anon";
    default:
        return "unknown";
    }
}

static int append_memid(memid_list_t *list, uint64_t memid)
{
    if ((memid == 0) || (list->count >= OBMM_MAX_MEMIDS)) {
        return -1;
    }

    list->values[list->count++] = memid;
    return 0;
}

static int parse_memid_token(memid_list_t *list, const char *tok)
{
    const char *dash;
    char        start_buf[64];
    uint64_t    start;
    uint64_t    end;
    uint64_t    memid;
    size_t      len;

    dash = strchr(tok, '-');
    if (dash == NULL) {
        return (parse_u64(tok, &memid) == 0) ? append_memid(list, memid) : -1;
    }

    if ((dash == tok) || (dash[1] == '\0') || (strchr(dash + 1, '-') != NULL)) {
        return -1;
    }

    len = (size_t)(dash - tok);
    if (len >= sizeof(start_buf)) {
        return -1;
    }

    memcpy(start_buf, tok, len);
    start_buf[len] = '\0';

    if ((parse_u64(start_buf, &start) != 0) ||
        (parse_u64(dash + 1, &end) != 0) || (end < start)) {
        return -1;
    }

    for (memid = start; memid <= end; ++memid) {
        if (append_memid(list, memid) != 0) {
            return -1;
        }
        if (memid == UINT64_MAX) {
            break;
        }
    }

    return 0;
}

static int parse_memid_tokens(memid_list_t *list, char *text)
{
    const char *delim = " ,\t\r\n";
    char       *saveptr = NULL;
    char       *comment;
    char       *tok;

    comment = strchr(text, '#');
    if (comment != NULL) {
        *comment = '\0';
    }

    for (tok = strtok_r(text, delim, &saveptr); tok != NULL;
         tok = strtok_r(NULL, delim, &saveptr)) {
        if (parse_memid_token(list, tok) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_memids_arg(memid_list_t *list, const char *arg)
{
    char *tmp = strdup(arg);
    int   ret;

    if (tmp == NULL) {
        return -1;
    }

    ret = parse_memid_tokens(list, tmp);
    free(tmp);
    return ret;
}

static int parse_memid_file(memid_list_t *list, const char *path)
{
    char  line[4096];
    FILE *f;

    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        if (parse_memid_tokens(list, line) != 0) {
            fprintf(stderr, "failed to parse memid file line: %s\n", line);
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

static int fill_default_memids(memid_list_t *list)
{
    uint64_t memid;
    size_t   i;

    for (i = 0; i < OBMM_DEFAULT_MEMID_COUNT; ++i) {
        memid = OBMM_DEFAULT_FIRST_MEMID + (uint64_t)i;
        if (append_memid(list, memid) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_opts(int argc, char **argv, probe_opts_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->mode        = MODE_CAS;
    opts->layout      = LAYOUT_SAME;
    opts->backend     = BACKEND_OBMM_NC;
    opts->offset      = OBMM_DEFAULT_OFFSET;
    opts->color_step  = OBMM_DEFAULT_COLOR_STEP;
    opts->bytes       = sizeof(uint64_t);
    opts->seconds     = OBMM_DEFAULT_SECONDS;
    opts->start_delay = OBMM_DEFAULT_START_DELAY;
    opts->timeout     = OBMM_DEFAULT_TIMEOUT;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--memids") && (i + 1 < argc)) {
            if (parse_memids_arg(&opts->memids, argv[++i]) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--memid-file") && (i + 1 < argc)) {
            if (parse_memid_file(&opts->memids, argv[++i]) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--mode") && (i + 1 < argc)) {
            if (parse_mode(argv[++i], &opts->mode) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--backend") && (i + 1 < argc)) {
            if (parse_backend(argv[++i], &opts->backend) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--layout") && (i + 1 < argc)) {
            if (parse_layout(argv[++i], &opts->layout) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--colors") && (i + 1 < argc)) {
            if (parse_size(argv[++i], &opts->colors) != 0) {
                return -1;
            }
            opts->have_colors = 1;
        } else if (!strcmp(argv[i], "--offset") && (i + 1 < argc)) {
            if (parse_u64(argv[++i], &opts->offset) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--color-step") && (i + 1 < argc)) {
            if (parse_u64(argv[++i], &opts->color_step) != 0) {
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
        } else if (!strcmp(argv[i], "--start-delay") && (i + 1 < argc)) {
            if (parse_double(argv[++i], &opts->start_delay) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--timeout") && (i + 1 < argc)) {
            if (parse_double(argv[++i], &opts->timeout) != 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "--fence-each")) {
            opts->fence_each = 1;
        } else if (!strcmp(argv[i], "--allow-shared-memid")) {
            opts->allow_shared_memid = 1;
        } else if (!strcmp(argv[i], "--per-rank")) {
            opts->per_rank = 1;
        } else if (!strcmp(argv[i], "--summary-file") && (i + 1 < argc)) {
            if (snprintf(opts->summary_file, sizeof(opts->summary_file), "%s",
                         argv[++i]) >= (int)sizeof(opts->summary_file)) {
                return -1;
            }
            opts->have_summary_file = 1;
        } else {
            return -1;
        }
    }

    if (opts->memids.count == 0) {
        if (fill_default_memids(&opts->memids) != 0) {
            return -1;
        }
    }

    if ((opts->bytes == 0) || ((opts->offset % sizeof(uint64_t)) != 0) ||
        ((opts->color_step % sizeof(uint64_t)) != 0)) {
        return -1;
    }

    if (opts->have_colors && (opts->colors == 0)) {
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

static void nc_load_barrier(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dmb oshld" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("" ::: "memory");
#elif defined(__powerpc64__)
    __asm__ __volatile__("lwsync" ::: "memory");
#elif defined(__riscv) && (__riscv_xlen == 64)
    __asm__ __volatile__("fence ir, ir" ::: "memory");
#else
    __sync_synchronize();
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

static uint64_t nc_cswap64(volatile uint64_t *ptr, uint64_t oldval,
                           uint64_t newval)
{
#if defined(__aarch64__)
    __asm__ __volatile__(
            ".arch_extension lse\n\t"
            "cas %x[old], %x[new], [%[ptr]]"
            : [old] "+r" (oldval)
            : [new] "r" (newval), [ptr] "r" (ptr)
            : "memory");
    return oldval;
#else
    return __sync_val_compare_and_swap(ptr, oldval, newval);
#endif
}

static uint64_t align_down_u64(uint64_t value, uint64_t align)
{
    return value & ~(align - 1u);
}

static uint64_t align_up_u64(uint64_t value, uint64_t align)
{
    return (value + align - 1u) & ~(align - 1u);
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

static int open_obmm_map(uint64_t memid, uint64_t offset, uint64_t access_size,
                         probe_backend_t backend, probe_map_t *map)
{
    char     dev_path[PATH_MAX];
    uint64_t region_size;
    uint64_t page_size;
    uint64_t map_offset;
    uint64_t map_delta;
    uint64_t map_length;
    int      fd;
    int      flags;
    void    *ptr;

    if (read_region_size(memid, &region_size) != 0) {
        return -1;
    }

    if ((offset > region_size) || (access_size > (region_size - offset))) {
        fprintf(stderr,
                "region too small: memid=%" PRIu64 " size=%" PRIu64
                " offset=%" PRIu64 " access=%" PRIu64 "\n",
                memid, region_size, offset, access_size);
        return -1;
    }

    page_size  = (uint64_t)sysconf(_SC_PAGESIZE);
    map_offset = align_down_u64(offset, page_size);
    map_delta  = offset - map_offset;
    map_length = align_up_u64(map_delta + access_size, page_size);
    if ((map_offset > region_size) ||
        (map_length > (region_size - map_offset))) {
        fprintf(stderr,
                "map range too large: memid=%" PRIu64 " size=%" PRIu64
                " map_offset=%" PRIu64 " map_length=%" PRIu64 "\n",
                memid, region_size, map_offset, map_length);
        return -1;
    }

    snprintf(dev_path, sizeof(dev_path), OBMM_DEV_FMT, memid);
    flags = O_RDWR;
    if (backend == BACKEND_OBMM_NC) {
        flags |= O_SYNC;
    }

    fd = open(dev_path, flags);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev_path, strerror(errno));
        return -1;
    }

    ptr = mmap(NULL, (size_t)map_length, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, (off_t)map_offset);
    if (ptr == MAP_FAILED) {
        fprintf(stderr,
                "mmap(%s, offset=%" PRIu64 " size=%" PRIu64 ") failed: %s\n",
                dev_path, map_offset, map_length, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    map->base       = ptr;
    map->map_offset = map_offset;
    map->map_length = map_length;
    return 0;
}

static int open_anon_map(uint64_t offset, uint64_t access_size,
                         probe_map_t *map)
{
    uint64_t page_size;
    uint64_t map_length;
    void    *ptr;

    page_size  = (uint64_t)sysconf(_SC_PAGESIZE);
    if (access_size > (UINT64_MAX - offset)) {
        fprintf(stderr,
                "anonymous mapping size overflow: offset=%" PRIu64
                " access=%" PRIu64 "\n",
                offset, access_size);
        return -1;
    }

    map_length = offset + access_size;
    if (map_length > (UINT64_MAX - (page_size - 1u))) {
        fprintf(stderr,
                "anonymous mapping size overflow after alignment: "
                "offset=%" PRIu64 " access=%" PRIu64 "\n",
                offset, access_size);
        return -1;
    }
    map_length = align_up_u64(map_length, page_size);

    ptr = mmap(NULL, (size_t)map_length, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr,
                "anonymous mmap(offset=%" PRIu64 " size=%" PRIu64
                ") failed: %s\n",
                offset, map_length, strerror(errno));
        return -1;
    }

    map->base       = ptr;
    map->map_offset = 0;
    map->map_length = map_length;

    /* Fault in the target page so load mode does not benchmark the shared
     * kernel zero page. */
    *(volatile uint64_t*)(map->base + offset) = 0;
    __sync_synchronize();
    return 0;
}

static uint64_t access_offset(const probe_opts_t *opts, size_t index,
                              size_t default_color_count,
                              size_t *color_count_p, size_t *color_index_p)
{
    size_t color_count;
    size_t color_index;

    if (opts->layout == LAYOUT_SAME) {
        color_count = 1;
    } else if (opts->have_colors) {
        color_count = opts->colors;
    } else {
        color_count = default_color_count;
    }

    if (color_count == 0) {
        color_count = 1;
    }

    color_index = index % color_count;
    *color_count_p = color_count;
    *color_index_p = color_index;
    return opts->offset + ((uint64_t)color_index * opts->color_step);
}

static const char *first_env_string(const char **names)
{
    int         i;
    const char *s;

    for (i = 0; names[i] != NULL; ++i) {
        s = getenv(names[i]);
        if ((s != NULL) && (*s != '\0')) {
            return s;
        }
    }

    return "default";
}

static void sanitize_path_component(const char *src, char *dst, size_t dst_len)
{
    size_t i;
    size_t j = 0;
    char   c;

    if (dst_len == 0) {
        return;
    }

    for (i = 0; (src[i] != '\0') && (j + 1 < dst_len); ++i) {
        c = src[i];
        if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
            ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-') ||
            (c == '_')) {
            dst[j++] = c;
        } else {
            dst[j++] = '_';
        }
    }

    dst[j] = '\0';
}

static int build_summary_path(const probe_opts_t *opts, char *path,
                              size_t path_len)
{
    static const char *summary_envs[] = {
        "OBMM_PROBE_SUMMARY_ID", "PMIX_NAMESPACE", "OMPI_COMM_WORLD_JOBID",
        "SLURM_JOB_ID", "PMI_JOBID", NULL
    };
    char        host[128];
    char        safe_host[128];
    char        safe_id[128];
    const char *id;

    if (opts->have_summary_file) {
        return (snprintf(path, path_len, "%s", opts->summary_file) <
                (int)path_len) ? 0 : -1;
    }

    if (gethostname(host, sizeof(host)) != 0) {
        snprintf(host, sizeof(host), "unknown");
    }
    host[sizeof(host) - 1] = '\0';

    id = first_env_string(summary_envs);
    sanitize_path_component(host, safe_host, sizeof(safe_host));
    sanitize_path_component(id, safe_id, sizeof(safe_id));

    return (snprintf(path, path_len,
                     "/tmp/obmm_offset_pressure_%lu_%s_%s.dat",
                     (unsigned long)getuid(), safe_host, safe_id) <
            (int)path_len) ? 0 : -1;
}

static size_t summary_length(int local_size)
{
    return sizeof(summary_hdr_t) +
           ((size_t)local_size * sizeof(summary_slot_t));
}

static int summary_wait_ready(summary_map_t *summary, int local_size)
{
    double deadline = now_sec() + OBMM_SUMMARY_WAIT_SEC;

    while (now_sec() < deadline) {
        __sync_synchronize();
        if ((summary->hdr->ready == 1u) &&
            (summary->hdr->magic == OBMM_SUMMARY_MAGIC) &&
            (summary->hdr->version == OBMM_SUMMARY_VERSION) &&
            (summary->hdr->local_size == (uint32_t)local_size)) {
            return 0;
        }
        usleep(1000);
    }

    fprintf(stderr, "summary mmap header did not become ready: %s\n",
            summary->path);
    return -1;
}

static int summary_open_map(const probe_opts_t *opts, int local_rank,
                            int local_size, summary_map_t *summary)
{
    double deadline;
    struct stat st;
    int    fd = -1;
    void  *ptr;

    memset(summary, 0, sizeof(*summary));
    if (opts->per_rank) {
        return 0;
    }

    if (build_summary_path(opts, summary->path, sizeof(summary->path)) != 0) {
        fprintf(stderr, "failed to build summary file path\n");
        return -1;
    }

    summary->length = summary_length(local_size);

    if (local_rank == 0) {
        if ((unlink(summary->path) != 0) && (errno != ENOENT)) {
            fprintf(stderr, "unlink(%s) failed: %s\n", summary->path,
                    strerror(errno));
            return -1;
        }

        fd = open(summary->path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            fprintf(stderr, "open(%s) failed: %s\n", summary->path,
                    strerror(errno));
            return -1;
        }

        if (ftruncate(fd, (off_t)summary->length) != 0) {
            fprintf(stderr, "ftruncate(%s) failed: %s\n", summary->path,
                    strerror(errno));
            close(fd);
            unlink(summary->path);
            return -1;
        }
    } else {
        deadline = now_sec() + OBMM_SUMMARY_WAIT_SEC;
        while (now_sec() < deadline) {
            fd = open(summary->path, O_RDWR);
            if (fd >= 0) {
                if ((fstat(fd, &st) == 0) &&
                    ((size_t)st.st_size >= summary->length)) {
                    break;
                }
                close(fd);
                fd = -1;
                usleep(1000);
                continue;
            }
            if (errno != ENOENT) {
                fprintf(stderr, "open(%s) failed: %s\n", summary->path,
                        strerror(errno));
                return -1;
            }
            usleep(1000);
        }
        if (fd < 0) {
            fprintf(stderr, "timed out waiting for summary file: %s\n",
                    summary->path);
            return -1;
        }
    }

    ptr = mmap(NULL, summary->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
               0);
    close(fd);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap(%s) failed: %s\n", summary->path,
                strerror(errno));
        if (local_rank == 0) {
            unlink(summary->path);
        }
        return -1;
    }

    summary->active = 1;
    summary->hdr    = (summary_hdr_t*)ptr;
    summary->slots  = (summary_slot_t*)((char*)ptr + sizeof(summary_hdr_t));

    if (local_rank == 0) {
        memset(ptr, 0, summary->length);
        summary->hdr->magic       = OBMM_SUMMARY_MAGIC;
        summary->hdr->version     = OBMM_SUMMARY_VERSION;
        summary->hdr->local_size  = (uint32_t)local_size;
        summary->hdr->header_size = (uint32_t)sizeof(summary_hdr_t);
        __sync_synchronize();
        summary->hdr->ready = 1u;
    }

    if (summary_wait_ready(summary, local_size) != 0) {
        munmap(summary->hdr, summary->length);
        if (local_rank == 0) {
            unlink(summary->path);
        }
        memset(summary, 0, sizeof(*summary));
        return -1;
    }

    return 0;
}

static void summary_close(summary_map_t *summary, int local_rank)
{
    if (!summary->active) {
        return;
    }

    munmap(summary->hdr, summary->length);
    if (local_rank == 0) {
        unlink(summary->path);
    }
    memset(summary, 0, sizeof(*summary));
}

static void summary_store(summary_map_t *summary, int local_rank,
                          const summary_slot_t *result)
{
    summary_slot_t *slot;

    if (!summary->active) {
        return;
    }

    slot = &summary->slots[local_rank];
    *slot = *result;
    __sync_synchronize();
    slot->state = 1u;
    __sync_synchronize();
}

static void print_rank_result(const probe_opts_t *opts,
                              const summary_slot_t *result, int size,
                              int local_size)
{
    printf("OBMM_OFFSET_PRESSURE rank=%d size=%d local_rank=%d local_size=%d "
           "backend=%s mode=%s role=%s layout=%s colors=%" PRIu64
           " color_index=%" PRIu64 " memid=%" PRIu64
           " memid_index=%" PRIu64 " offset=%" PRIu64
           " offset_low_2m=0x%" PRIx64 " color_step=%" PRIu64
           " bytes=%zu fence_each=%d ops=%" PRIu64 " seconds=%.6f "
           "ops_per_sec=%.3f ns_per_op=%.3f aux=%" PRIu64
           " checksum=%" PRIu64 " status=%s\n",
           result->rank, size, result->local_rank, local_size,
           backend_name(opts->backend), mode_name(opts->mode),
           (result->role == 1) ? "writer" :
           ((result->role == 2) ? "reader" : "single"),
           layout_name(opts->layout), result->color_count,
           result->color_index, result->memid, result->memid_index,
           result->offset, result->offset & (OBMM_2M - 1u),
           opts->color_step, opts->bytes, opts->fence_each, result->ops,
           result->elapsed, result->ops_per_sec, result->ns_per_op,
           result->aux, result->checksum,
           result->status ? "FAIL" : "OK");
}

static void summary_print(summary_map_t *summary, const probe_opts_t *opts,
                          int local_size)
{
    double          deadline = now_sec() + OBMM_SUMMARY_WAIT_SEC;
    double          sum_ops_per_sec = 0.0;
    double          sum_ns_per_op = 0.0;
    double          min_ns_per_op = DBL_MAX;
    double          max_ns_per_op = 0.0;
    double          sum_elapsed = 0.0;
    double          avg_ops_per_sec;
    double          avg_ns_per_op;
    double          avg_elapsed;
    double          avg_aux;
    uint64_t        total_ops = 0;
    uint64_t        total_aux = 0;
    uint64_t        checksum = 0;
    uint64_t        colors = 0;
    unsigned        ready;
    unsigned        ok = 0;
    unsigned        failed = 0;
    unsigned        writers = 0;
    unsigned        readers = 0;
    unsigned        singles = 0;
    int             i;
    summary_slot_t *slot;

    do {
        ready = 0;
        for (i = 0; i < local_size; ++i) {
            __sync_synchronize();
            if (summary->slots[i].state == 1u) {
                ++ready;
            }
        }
        if (ready == (unsigned)local_size) {
            break;
        }
        usleep(1000);
    } while (now_sec() < deadline);

    for (i = 0; i < local_size; ++i) {
        slot = &summary->slots[i];
        if (slot->state != 1u) {
            continue;
        }

        if (slot->role == 1) {
            ++writers;
        } else if (slot->role == 2) {
            ++readers;
        } else {
            ++singles;
        }

        if (colors == 0) {
            colors = slot->color_count;
        }

        if (slot->status != 0) {
            ++failed;
            continue;
        }

        ++ok;
        total_ops       += slot->ops;
        total_aux       += slot->aux;
        checksum        += slot->checksum;
        sum_elapsed     += slot->elapsed;
        sum_ops_per_sec += slot->ops_per_sec;
        sum_ns_per_op   += slot->ns_per_op;
        if (slot->ns_per_op < min_ns_per_op) {
            min_ns_per_op = slot->ns_per_op;
        }
        if (slot->ns_per_op > max_ns_per_op) {
            max_ns_per_op = slot->ns_per_op;
        }
    }

    failed += (unsigned)local_size - ready;
    avg_ops_per_sec = (ok == 0) ? 0.0 : (sum_ops_per_sec / (double)ok);
    avg_ns_per_op   = (ok == 0) ? 0.0 : (sum_ns_per_op / (double)ok);
    avg_elapsed     = (ok == 0) ? 0.0 : (sum_elapsed / (double)ok);
    avg_aux         = (ok == 0) ? 0.0 : ((double)total_aux / (double)ok);
    if (ok == 0) {
        min_ns_per_op = 0.0;
    }

    printf("OBMM_OFFSET_PRESSURE_SUMMARY local_size=%d backend=%s mode=%s "
           "layout=%s colors=%" PRIu64 " ready=%u ok=%u failed=%u writers=%u "
           "readers=%u singles=%u bytes=%zu fence_each=%d color_step=%" PRIu64
           " total_ops=%" PRIu64 " avg_ops_per_sec=%.3f "
           "total_ops_per_sec=%.3f avg_ns_per_op=%.3f "
           "min_ns_per_op=%.3f max_ns_per_op=%.3f avg_aux=%.3f "
           "avg_seconds=%.6f checksum=%" PRIu64 " status=%s\n",
           local_size, backend_name(opts->backend), mode_name(opts->mode),
           layout_name(opts->layout),
           colors, ready, ok, failed, writers, readers, singles, opts->bytes,
           opts->fence_each, opts->color_step, total_ops, avg_ops_per_sec,
           sum_ops_per_sec, avg_ns_per_op, min_ns_per_op, max_ns_per_op,
           avg_aux, avg_elapsed, checksum, failed ? "FAIL" : "OK");
}

static void fill_payload(uint8_t *payload, size_t bytes, uint64_t seq, int rank)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        payload[i] = (uint8_t)(seq + (uint64_t)i + (uint64_t)rank);
    }
}

static uint64_t checksum_payload(const volatile uint8_t *payload, size_t bytes)
{
    uint64_t sum = 0;
    size_t   i;

    for (i = 0; i < bytes; ++i) {
        sum += payload[i];
    }

    return sum;
}

static int run_load(volatile uint64_t *word, double start, double end,
                    int fence_each, uint64_t *ops_p, uint64_t *checksum_p)
{
    uint64_t ops = 0;
    uint64_t checksum = 0;

    while (now_sec() < start) {
        /* spin to align independently launched ranks */
    }

    while (now_sec() < end) {
        checksum += *word;
        if (fence_each) {
            nc_load_barrier();
        }
        ++ops;
    }

    *ops_p      = ops;
    *checksum_p = checksum;
    return 0;
}

static int run_store(volatile uint64_t *word, double start, double end,
                     int fence_each, int rank, uint64_t *ops_p)
{
    uint64_t ops = 0;

    *word = 0;
    nc_full_barrier();
    while (now_sec() < start) {
        /* spin to align independently launched ranks */
    }

    while (now_sec() < end) {
        *word = ((uint64_t)rank << 48) | ops;
        if (fence_each) {
            nc_full_barrier();
        }
        ++ops;
    }

    nc_full_barrier();
    *ops_p = ops;
    return 0;
}

static int run_cas(volatile uint64_t *word, double start, double end,
                   uint64_t *ops_p, uint64_t *retries_p)
{
    uint64_t ops = 0;
    uint64_t retries = 0;
    uint64_t oldval;
    uint64_t prev;

    *word = 0;
    nc_full_barrier();
    while (now_sec() < start) {
        /* spin to align independently launched ranks */
    }

    oldval = *word;
    while (now_sec() < end) {
        prev = nc_cswap64(word, oldval, oldval + 1u);
        if (prev == oldval) {
            ++ops;
            ++oldval;
        } else {
            ++retries;
            oldval = prev;
        }
    }

    *ops_p     = ops;
    *retries_p = retries;
    return 0;
}

static int run_handoff_writer(handoff_record_t *rec, size_t bytes,
                              double start, double end, double timeout,
                              int rank, uint64_t *ops_p, uint64_t *spins_p)
{
    uint64_t ops = 0;
    uint64_t spins = 0;
    uint64_t seq = 1;

    rec->ready = 0;
    rec->ack   = 0;
    nc_full_barrier();

    while (now_sec() < start) {
        /* spin to align independently launched ranks */
    }

    while (now_sec() < end) {
        double wait_until = now_sec() + timeout;

        while (rec->ack != (seq - 1u)) {
            ++spins;
            if (now_sec() > wait_until) {
                fprintf(stderr,
                        "handoff writer timed out waiting for ack=%" PRIu64
                        " observed=%" PRIu64 "\n",
                        seq - 1u, rec->ack);
                rec->ready = OBMM_DONE_VALUE;
                nc_full_barrier();
                return -1;
            }
        }

        fill_payload(rec->payload, bytes, seq, rank);
        nc_full_barrier();
        rec->ready = seq;

        ++ops;
        ++seq;
    }

    while (rec->ack != (seq - 1u)) {
        if (now_sec() > (end + timeout)) {
            fprintf(stderr,
                    "handoff writer timed out waiting for final ack=%" PRIu64
                    " observed=%" PRIu64 "\n",
                    seq - 1u, rec->ack);
            rec->ready = OBMM_DONE_VALUE;
            nc_full_barrier();
            return -1;
        }
        ++spins;
    }

    nc_full_barrier();
    rec->ready = OBMM_DONE_VALUE;
    nc_full_barrier();

    *ops_p   = ops;
    *spins_p = spins;
    return 0;
}

static int run_handoff_reader(handoff_record_t *rec, size_t bytes,
                              double start, double timeout, uint64_t *ops_p,
                              uint64_t *spins_p, uint64_t *checksum_p)
{
    uint64_t ops = 0;
    uint64_t spins = 0;
    uint64_t checksum = 0;
    uint64_t seq = 1;

    while (now_sec() < start) {
        /* spin to align independently launched ranks */
    }

    for (;;) {
        double wait_until = now_sec() + timeout;
        uint64_t ready;

        do {
            ready = rec->ready;
            if ((ready == OBMM_DONE_VALUE) && (ops > 0)) {
                *ops_p      = ops;
                *spins_p    = spins;
                *checksum_p = checksum;
                return 0;
            }
            ++spins;
            if (now_sec() > wait_until) {
                fprintf(stderr,
                        "handoff reader timed out waiting for ready=%" PRIu64
                        " observed=%" PRIu64 "\n",
                        seq, ready);
                return -1;
            }
        } while (ready != seq);

        nc_load_barrier();
        checksum += checksum_payload(rec->payload, bytes);
        nc_full_barrier();
        rec->ack = seq;

        ++ops;
        ++seq;
    }
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
        "MPI_LOCALNRANKS", "SLURM_NTASKS_PER_NODE", NULL
    };

    probe_opts_t opts;
    probe_map_t  map;
    summary_map_t summary;
    summary_slot_t result;
    uint64_t     access_size;
    uint64_t     access;
    uint64_t     offset;
    uint64_t     memid;
    uint64_t     ops = 0;
    uint64_t     aux = 0;
    uint64_t     checksum = 0;
    size_t       default_color_count;
    size_t       memid_index;
    size_t       color_count;
    size_t       color_index;
    double       start;
    double       end;
    double       elapsed;
    int          rank, size, local_rank, local_size;
    int          role = 0;
    int          status = 0;
    int          ret;

    memset(&map, 0, sizeof(map));
    memset(&summary, 0, sizeof(summary));
    memset(&result, 0, sizeof(result));

    if (parse_opts(argc, argv, &opts) != 0) {
        usage(argv[0]);
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

    if ((opts.backend == BACKEND_ANON) && (opts.mode == MODE_HANDOFF)) {
        fprintf(stderr,
                "anonymous backend does not support handoff; use load, "
                "store, or cas\n");
        return 1;
    }

    if (opts.mode == MODE_HANDOFF) {
        if ((local_size % 2) != 0) {
            fprintf(stderr, "handoff mode requires even local_size\n");
            return 1;
        }
        memid_index = (size_t)(local_rank % (local_size / 2));
        role        = (local_rank < (local_size / 2)) ? 1 : 2;
        if (!opts.allow_shared_memid &&
            ((size_t)(local_size / 2) > opts.memids.count)) {
            fprintf(stderr,
                    "handoff pairs (%d) exceed memid count (%zu); refusing "
                    "shared physical words without --allow-shared-memid\n",
                    local_size / 2, opts.memids.count);
            return 1;
        }
    } else {
        memid_index = (size_t)local_rank;
        if ((opts.backend != BACKEND_ANON) && !opts.allow_shared_memid &&
            ((size_t)local_size > opts.memids.count)) {
            fprintf(stderr,
                    "local ranks (%d) exceed memid count (%zu); refusing "
                    "shared physical words without --allow-shared-memid\n",
                    local_size, opts.memids.count);
            return 1;
        }
    }

    if (opts.backend != BACKEND_ANON) {
        memid_index %= opts.memids.count;
        memid               = opts.memids.values[memid_index];
        default_color_count = opts.memids.count;
    } else {
        memid               = 0;
        default_color_count = (size_t)local_size;
    }

    offset = access_offset(&opts, memid_index, default_color_count,
                           &color_count, &color_index);

    access_size = (opts.mode == MODE_HANDOFF) ?
                  (sizeof(handoff_record_t) + opts.bytes) :
                  sizeof(uint64_t);

    if (summary_open_map(&opts, local_rank, local_size, &summary) != 0) {
        return 1;
    }

    result.rank        = rank;
    result.local_rank  = local_rank;
    result.role        = role;
    result.memid       = memid;
    result.offset      = offset;
    result.memid_index = (uint64_t)memid_index;
    result.color_count = (uint64_t)color_count;
    result.color_index = (uint64_t)color_index;

    if (((opts.backend != BACKEND_ANON) ?
         open_obmm_map(memid, offset, access_size, opts.backend, &map) :
         open_anon_map(offset, access_size, &map)) != 0) {
        result.status = 1;
        if (opts.per_rank) {
            print_rank_result(&opts, &result, size, local_size);
        } else {
            summary_store(&summary, local_rank, &result);
            if (local_rank == 0) {
                summary_print(&summary, &opts, local_size);
            }
            summary_close(&summary, local_rank);
        }
        return 1;
    }

    access = offset - map.map_offset;
    start  = now_sec() + opts.start_delay;
    end    = start + opts.seconds;

    switch (opts.mode) {
    case MODE_LOAD:
        ret = run_load((volatile uint64_t*)(map.base + access), start, end,
                       opts.fence_each, &ops, &checksum);
        break;
    case MODE_STORE:
        ret = run_store((volatile uint64_t*)(map.base + access), start, end,
                        opts.fence_each, rank, &ops);
        break;
    case MODE_CAS:
        ret = run_cas((volatile uint64_t*)(map.base + access), start, end,
                      &ops, &aux);
        break;
    case MODE_HANDOFF:
        if (role == 1) {
            ret = run_handoff_writer((handoff_record_t*)(map.base + access),
                                     opts.bytes, start, end, opts.timeout,
                                     rank, &ops, &aux);
        } else {
            ret = run_handoff_reader((handoff_record_t*)(map.base + access),
                                     opts.bytes, start, opts.timeout, &ops,
                                     &aux, &checksum);
        }
        break;
    default:
        ret = -1;
        break;
    }

    elapsed = now_sec() - start;
    if (elapsed <= 0.0) {
        elapsed = opts.seconds;
    }
    status = (ret == 0) ? 0 : 1;

    result.status      = (uint32_t)status;
    result.ops         = ops;
    result.aux         = aux;
    result.checksum    = checksum;
    result.elapsed     = elapsed;
    result.ops_per_sec = ops / elapsed;
    result.ns_per_op   = (ops == 0) ? 0.0 : (elapsed * 1e9 / (double)ops);

    if (opts.per_rank) {
        print_rank_result(&opts, &result, size, local_size);
    } else {
        summary_store(&summary, local_rank, &result);
        if (local_rank == 0) {
            summary_print(&summary, &opts, local_size);
        }
        summary_close(&summary, local_rank);
    }

    munmap(map.base, (size_t)map.map_length);
    return status;
}
