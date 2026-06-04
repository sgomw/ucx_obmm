/*
 * Standalone cacheable-CC ownership timing probe for obmm.
 *
 * The probe opens one /dev/obmm_shmdev* without O_SYNC, maps it with
 * PROT_NONE, and measures obmm_set_ownership() transitions for ranges that
 * mimic the current sender-staged AM_ZCOPY path.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ARRAY_SIZE(_a) (sizeof(_a) / sizeof((_a)[0]))
#define NSEC_PER_USEC 1000.0

typedef int (*obmm_set_ownership_fn_t)(int fd, void *start, void *end,
                                       int prot);

typedef enum {
    MODE_BOTH,
    MODE_WRITE,
    MODE_READ,
    MODE_EMPTY
} probe_mode_t;

typedef struct {
    const char *name;
    size_t      offset;
    size_t      own_len;
    size_t      dirty_len;
} probe_case_t;

typedef struct {
    uint64_t write_acq;
    uint64_t dirty;
    uint64_t write_none;
    uint64_t read_acq;
    uint64_t touch;
    uint64_t read_none;
    uint64_t total;
} sample_t;

typedef struct {
    char         dev_path[PATH_MAX];
    uint64_t     memid;
    int          have_memid;
    size_t       map_size;
    unsigned     iters;
    unsigned     warmup;
    probe_mode_t mode;
    int          no_chunked;
} options_t;

static volatile uint64_t g_sink;

static uint64_t now_nsec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;

    return (va > vb) - (va < vb);
}

static uint64_t median_u64(uint64_t *values, unsigned count)
{
    qsort(values, count, sizeof(values[0]), cmp_u64);
    return values[count / 2];
}

static double usec(uint64_t nsec)
{
    return (double)nsec / NSEC_PER_USEC;
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int parse_size(const char *str, size_t *value_p)
{
    char          *end;
    unsigned long long value;
    unsigned long long scale = 1;

    errno = 0;
    value = strtoull(str, &end, 0);
    if ((errno != 0) || (end == str)) {
        return -1;
    }

    if ((*end == 'K') || (*end == 'k')) {
        scale = 1024ull;
        ++end;
    } else if ((*end == 'M') || (*end == 'm')) {
        scale = 1024ull * 1024ull;
        ++end;
    } else if ((*end == 'G') || (*end == 'g')) {
        scale = 1024ull * 1024ull * 1024ull;
        ++end;
    }

    if (*end != '\0') {
        return -1;
    }

    if (value > (ULLONG_MAX / scale)) {
        return -1;
    }

    *value_p = (size_t)(value * scale);
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--memid ID | --dev PATH] [options]\n"
            "\n"
            "Options:\n"
            "  --iters N        measured iterations per case (default: 30)\n"
            "  --warmup N       warmup iterations per case (default: 3)\n"
            "  --map-size SIZE  mmap length (default: 16M)\n"
            "  --mode MODE      both|write|read|empty (default: both)\n"
            "  --no-chunked     skip staged-zcopy chunk emulation table\n"
            "  --help           show this message\n"
            "\n"
            "If --memid/--dev is omitted, the first memid from\n"
            "UCX_OBMM_CC_MEMIDS is used.\n",
            prog);
}

static int parse_mode(const char *str, probe_mode_t *mode_p)
{
    if (strcmp(str, "both") == 0) {
        *mode_p = MODE_BOTH;
    } else if (strcmp(str, "write") == 0) {
        *mode_p = MODE_WRITE;
    } else if (strcmp(str, "read") == 0) {
        *mode_p = MODE_READ;
    } else if (strcmp(str, "empty") == 0) {
        *mode_p = MODE_EMPTY;
    } else {
        return -1;
    }

    return 0;
}

static const char *mode_name(probe_mode_t mode)
{
    switch (mode) {
    case MODE_BOTH:
        return "both";
    case MODE_WRITE:
        return "write";
    case MODE_READ:
        return "read";
    case MODE_EMPTY:
        return "empty";
    }

    return "unknown";
}

static int parse_first_cc_memid(uint64_t *memid_p)
{
    const char *env = getenv("UCX_OBMM_CC_MEMIDS");
    char       *end;
    uint64_t    value;

    if ((env == NULL) || (*env == '\0')) {
        return -1;
    }

    errno = 0;
    value = strtoull(env, &end, 0);
    if ((errno != 0) || (end == env)) {
        return -1;
    }

    *memid_p = value;
    return 0;
}

static void options_init(options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->map_size = 16u * 1024u * 1024u;
    opts->iters    = 30;
    opts->warmup   = 3;
    opts->mode     = MODE_BOTH;
}

static void parse_args(int argc, char **argv, options_t *opts)
{
    int i;

    options_init(opts);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--memid") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            opts->memid      = strtoull(argv[i], NULL, 0);
            if (opts->memid == 0) {
                fprintf(stderr, "invalid memid: %s\n", argv[i]);
                exit(EXIT_FAILURE);
            }
            opts->have_memid = 1;
        } else if (strcmp(argv[i], "--dev") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            snprintf(opts->dev_path, sizeof(opts->dev_path), "%s", argv[i]);
        } else if (strcmp(argv[i], "--iters") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            opts->iters = (unsigned)strtoul(argv[i], NULL, 0);
        } else if (strcmp(argv[i], "--warmup") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            opts->warmup = (unsigned)strtoul(argv[i], NULL, 0);
        } else if (strcmp(argv[i], "--map-size") == 0) {
            if ((++i >= argc) || (parse_size(argv[i], &opts->map_size) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--mode") == 0) {
            if ((++i >= argc) || (parse_mode(argv[i], &opts->mode) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--no-chunked") == 0) {
            opts->no_chunked = 1;
        } else {
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if ((opts->iters == 0) || (opts->warmup >= opts->iters)) {
        fprintf(stderr, "invalid iteration counts\n");
        exit(EXIT_FAILURE);
    }

    if (opts->dev_path[0] == '\0') {
        if (!opts->have_memid && (parse_first_cc_memid(&opts->memid) == 0)) {
            opts->have_memid = 1;
        }

        if (!opts->have_memid) {
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }

        snprintf(opts->dev_path, sizeof(opts->dev_path),
                 "/dev/obmm_shmdev%" PRIu64, opts->memid);
    }
}

static obmm_set_ownership_fn_t load_set_ownership(void)
{
    void *handle;
    void *sym;

    handle = dlopen("libobmm.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        handle = dlopen("libobmm.so.0", RTLD_NOW | RTLD_LOCAL);
    }
    if (handle == NULL) {
        fprintf(stderr, "failed to dlopen libobmm.so/libobmm.so.0: %s\n",
                dlerror());
        exit(EXIT_FAILURE);
    }

    sym = dlsym(handle, "obmm_set_ownership");
    if (sym == NULL) {
        fprintf(stderr, "libobmm does not export obmm_set_ownership: %s\n",
                dlerror());
        exit(EXIT_FAILURE);
    }

    return (obmm_set_ownership_fn_t)sym;
}

static int set_owner(obmm_set_ownership_fn_t fn, int fd, void *base,
                     size_t offset, size_t length, int prot)
{
    void *start = (char*)base + offset;
    void *end   = (char*)start + length;

    return fn(fd, start, end, prot);
}

static void dirty_range(void *ptr, size_t length, unsigned iter)
{
    if (length == 0) {
        return;
    }

    memset(ptr, (int)(0xa5u ^ (iter & 0xffu)), length);
}

static void touch_range(const void *ptr, size_t length)
{
    const volatile uint8_t *p = (const volatile uint8_t*)ptr;
    uint64_t sum = 0;
    size_t   i;

    for (i = 0; i < length; i += 64) {
        sum += p[i];
    }
    if (length > 0) {
        sum += p[length - 1];
    }

    g_sink += sum;
}

static int run_one_sample(obmm_set_ownership_fn_t fn, int fd, void *map,
                          probe_mode_t mode, const probe_case_t *pc,
                          unsigned iter, sample_t *sample)
{
    void    *ptr = (char*)map + pc->offset;
    uint64_t t0, t1, t2, t3, t4, t5, t6;

    memset(sample, 0, sizeof(*sample));
    t0 = now_nsec();

    if ((mode == MODE_BOTH) || (mode == MODE_WRITE) || (mode == MODE_EMPTY)) {
        if (set_owner(fn, fd, map, pc->offset, pc->own_len, PROT_WRITE) != 0) {
            return -1;
        }
        t1 = now_nsec();

        if (mode != MODE_EMPTY) {
            dirty_range(ptr, pc->dirty_len, iter);
        }
        t2 = now_nsec();

        if (set_owner(fn, fd, map, pc->offset, pc->own_len, PROT_NONE) != 0) {
            return -1;
        }
        t3 = now_nsec();

        sample->write_acq  = t1 - t0;
        sample->dirty      = t2 - t1;
        sample->write_none = t3 - t2;
    } else {
        t3 = t0;
    }

    if ((mode == MODE_BOTH) || (mode == MODE_READ)) {
        if (set_owner(fn, fd, map, pc->offset, pc->own_len, PROT_READ) != 0) {
            return -1;
        }
        t4 = now_nsec();

        touch_range(ptr, pc->dirty_len);
        t5 = now_nsec();

        if (set_owner(fn, fd, map, pc->offset, pc->own_len, PROT_NONE) != 0) {
            return -1;
        }
        t6 = now_nsec();

        sample->read_acq  = t4 - t3;
        sample->touch     = t5 - t4;
        sample->read_none = t6 - t5;
    } else {
        t6 = t3;
    }

    sample->total = t6 - t0;
    return 0;
}

static uint64_t median_field(sample_t *samples, unsigned count,
                             uint64_t (*get)(const sample_t*))
{
    uint64_t *values;
    uint64_t  result;
    unsigned  i;

    values = malloc(sizeof(*values) * count);
    if (values == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < count; ++i) {
        values[i] = get(&samples[i]);
    }

    result = median_u64(values, count);
    free(values);
    return result;
}

static uint64_t get_write_acq(const sample_t *s)  { return s->write_acq; }
static uint64_t get_dirty(const sample_t *s)      { return s->dirty; }
static uint64_t get_write_none(const sample_t *s) { return s->write_none; }
static uint64_t get_read_acq(const sample_t *s)   { return s->read_acq; }
static uint64_t get_touch(const sample_t *s)      { return s->touch; }
static uint64_t get_read_none(const sample_t *s)  { return s->read_none; }
static uint64_t get_total(const sample_t *s)      { return s->total; }

static int run_case(obmm_set_ownership_fn_t fn, int fd, void *map,
                    size_t map_size, probe_mode_t mode, const char *section,
                    const probe_case_t *pc, unsigned iters, unsigned warmup,
                    double *write_none_median_p, double *total_median_p)
{
    sample_t *samples;
    sample_t  tmp;
    unsigned  i;
    unsigned  out = 0;
    int       rc;

    if ((pc->own_len == 0) || (pc->dirty_len > pc->own_len) ||
        (pc->offset > map_size) || (pc->own_len > (map_size - pc->offset))) {
        fprintf(stderr,
                "case %s/%s does not fit map: off=%zu own=%zu map=%zu\n",
                section, pc->name, pc->offset, pc->own_len, map_size);
        return -1;
    }

    samples = calloc(iters - warmup, sizeof(*samples));
    if (samples == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < iters; ++i) {
        rc = run_one_sample(fn, fd, map, mode, pc, i, &tmp);
        if (rc != 0) {
            fprintf(stderr,
                    "ERROR section=%s case=%s offset=%zu own_len=%zu "
                    "dirty_len=%zu errno=%d (%s)\n",
                    section, pc->name, pc->offset, pc->own_len,
                    pc->dirty_len, errno, strerror(errno));
            free(samples);
            return -1;
        }

        if (i >= warmup) {
            samples[out++] = tmp;
        }
    }

    printf("%s,%s,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
           section, pc->name, pc->offset, pc->own_len, pc->dirty_len,
           usec(median_field(samples, out, get_write_acq)),
           usec(median_field(samples, out, get_dirty)),
           usec(median_field(samples, out, get_write_none)),
           usec(median_field(samples, out, get_read_acq)),
           usec(median_field(samples, out, get_touch)),
           usec(median_field(samples, out, get_read_none)),
           usec(median_field(samples, out, get_total)));

    if (write_none_median_p != NULL) {
        *write_none_median_p = usec(median_field(samples, out, get_write_none));
    }
    if (total_median_p != NULL) {
        *total_median_p = usec(median_field(samples, out, get_total));
    }

    free(samples);
    return 0;
}

static int run_sweep(obmm_set_ownership_fn_t fn, int fd, void *map,
                     size_t map_size, probe_mode_t mode, unsigned iters,
                     unsigned warmup, size_t page_size)
{
    static const size_t sizes[] = {
        4u * 1024u,
        64u * 1024u,
        256u * 1024u,
        512u * 1024u,
        1024u * 1024u,
        2u * 1024u * 1024u,
        4u * 1024u * 1024u,
        6u * 1024u * 1024u
    };
    char         name[32];
    probe_case_t pc;
    size_t       own_len;
    unsigned     i;

    for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
        own_len = align_up(sizes[i], page_size);
        snprintf(name, sizeof(name), "%zu", sizes[i]);
        pc.name      = name;
        pc.offset    = 0;
        pc.own_len   = own_len;
        pc.dirty_len = sizes[i];
        if (run_case(fn, fd, map, map_size, mode, "sweep", &pc, iters,
                     warmup, NULL, NULL) != 0) {
            return -1;
        }
    }

    return 0;
}

static int run_boundary(obmm_set_ownership_fn_t fn, int fd, void *map,
                        size_t map_size, probe_mode_t mode, unsigned iters,
                        unsigned warmup, size_t page_size)
{
    const size_t gran = 2u * 1024u * 1024u;
    probe_case_t cases[] = {
        {"one_page_at_0",       0,                 page_size, page_size},
        {"one_page_before_2m",  gran - page_size,  page_size, page_size},
        {"cross_2m_2pages",    gran - page_size,  page_size * 2u,
                                                           page_size * 2u},
        {"exact_2m",           0,                 gran,      gran},
        {"two_m_plus_page",    0,                 gran + page_size,
                                                           gran + page_size}
    };
    double one_page_rel = 0.0;
    double cross_rel    = 0.0;
    double exact_rel    = 0.0;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(cases); ++i) {
        double *rel_p = NULL;

        if (strcmp(cases[i].name, "one_page_at_0") == 0) {
            rel_p = &one_page_rel;
        } else if (strcmp(cases[i].name, "cross_2m_2pages") == 0) {
            rel_p = &cross_rel;
        } else if (strcmp(cases[i].name, "exact_2m") == 0) {
            rel_p = &exact_rel;
        }

        if (run_case(fn, fd, map, map_size, mode, "boundary", &cases[i],
                     iters, warmup, rel_p, NULL) != 0) {
            return -1;
        }
    }

    if ((one_page_rel > 0.0) && (exact_rel > 0.0) && (cross_rel > 0.0)) {
        if ((one_page_rel > (exact_rel * 0.50)) ||
            (cross_rel > (one_page_rel * 1.50))) {
            printf("# inference: ownership cost likely PMD/2M-granular "
                   "(one_page_write_none=%.3fus exact_2m_write_none=%.3fus "
                   "cross_2m_write_none=%.3fus)\n",
                   one_page_rel, exact_rel, cross_rel);
        } else {
            printf("# inference: ownership cost looks closer to PAGE_SIZE "
                   "granularity (one_page_write_none=%.3fus "
                   "exact_2m_write_none=%.3fus cross_2m_write_none=%.3fus)\n",
                   one_page_rel, exact_rel, cross_rel);
        }
    }

    return 0;
}

static int run_chunked_one(obmm_set_ownership_fn_t fn, int fd, void *map,
                           size_t map_size, probe_mode_t mode,
                           size_t message_size, size_t chunk_size,
                           unsigned iters, unsigned warmup, size_t page_size)
{
    sample_t *samples;
    sample_t  tmp;
    sample_t  accum;
    unsigned  i;
    unsigned  out = 0;
    size_t    offset;
    size_t    remaining;
    size_t    frag_len;
    char      name[64];

    if (chunk_size > map_size) {
        return 0;
    }

    samples = calloc(iters - warmup, sizeof(*samples));
    if (samples == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < iters; ++i) {
        memset(&accum, 0, sizeof(accum));
        remaining = message_size;
        offset    = 0;

        while (remaining > 0) {
            probe_case_t pc;

            frag_len = (remaining < chunk_size) ? remaining : chunk_size;
            pc.name      = "chunk";
            pc.offset    = offset;
            pc.own_len   = align_up(chunk_size, page_size);
            pc.dirty_len = frag_len;

            if ((pc.offset + pc.own_len) > map_size) {
                fprintf(stderr,
                        "chunked case does not fit map: msg=%zu chunk=%zu "
                        "off=%zu map=%zu\n",
                        message_size, chunk_size, pc.offset, map_size);
                free(samples);
                return -1;
            }

            if (run_one_sample(fn, fd, map, mode, &pc, i, &tmp) != 0) {
                fprintf(stderr,
                        "ERROR section=chunked msg=%zu chunk=%zu errno=%d "
                        "(%s)\n",
                        message_size, chunk_size, errno, strerror(errno));
                free(samples);
                return -1;
            }

            accum.write_acq  += tmp.write_acq;
            accum.dirty      += tmp.dirty;
            accum.write_none += tmp.write_none;
            accum.read_acq   += tmp.read_acq;
            accum.touch      += tmp.touch;
            accum.read_none  += tmp.read_none;
            accum.total      += tmp.total;

            remaining -= frag_len;
            offset    += align_up(chunk_size, page_size);
        }

        if (i >= warmup) {
            samples[out++] = accum;
        }
    }

    snprintf(name, sizeof(name), "msg_%zu_chunk_%zu", message_size, chunk_size);
    printf("chunked,%s,0,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
           name, chunk_size, message_size,
           usec(median_field(samples, out, get_write_acq)),
           usec(median_field(samples, out, get_dirty)),
           usec(median_field(samples, out, get_write_none)),
           usec(median_field(samples, out, get_read_acq)),
           usec(median_field(samples, out, get_touch)),
           usec(median_field(samples, out, get_read_none)),
           usec(median_field(samples, out, get_total)));

    free(samples);
    return 0;
}

static int run_chunked(obmm_set_ownership_fn_t fn, int fd, void *map,
                       size_t map_size, probe_mode_t mode, unsigned iters,
                       unsigned warmup, size_t page_size)
{
    static const size_t messages[] = {
        256u * 1024u,
        512u * 1024u,
        1024u * 1024u,
        2u * 1024u * 1024u,
        4u * 1024u * 1024u
    };
    static const size_t chunks[] = {
        1024u * 1024u,
        2u * 1024u * 1024u,
        4u * 1024u * 1024u
    };
    unsigned i, j;

    for (i = 0; i < ARRAY_SIZE(messages); ++i) {
        for (j = 0; j < ARRAY_SIZE(chunks); ++j) {
            if (run_chunked_one(fn, fd, map, map_size, mode, messages[i],
                                chunks[j], iters, warmup, page_size) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    options_t               opts;
    obmm_set_ownership_fn_t set_ownership;
    long                    page_size_l;
    size_t                  page_size;
    int                     fd;
    void                   *map;
    int                     ret = EXIT_SUCCESS;

    parse_args(argc, argv, &opts);

    page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0) {
        fprintf(stderr, "failed to get page size\n");
        return EXIT_FAILURE;
    }
    page_size = (size_t)page_size_l;

    opts.map_size = align_up(opts.map_size, page_size);

    set_ownership = load_set_ownership();

    fd = open(opts.dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", opts.dev_path,
                strerror(errno));
        return EXIT_FAILURE;
    }

    map = mmap(NULL, opts.map_size, PROT_NONE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, %zu) failed: %s\n", opts.dev_path,
                opts.map_size, strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("# obmm_cc_ownership_probe dev=%s map_size=%zu page_size=%zu "
           "iters=%u warmup=%u mode=%s\n",
           opts.dev_path, opts.map_size, page_size, opts.iters, opts.warmup,
           mode_name(opts.mode));
    printf("section,case,offset,own_len,dirty_len,write_acq_us,dirty_us,"
           "write_none_us,read_acq_us,touch_us,read_none_us,total_us\n");

    if (run_sweep(set_ownership, fd, map, opts.map_size, opts.mode, opts.iters,
                  opts.warmup, page_size) != 0) {
        ret = EXIT_FAILURE;
        goto out;
    }

    if (run_boundary(set_ownership, fd, map, opts.map_size, opts.mode,
                     opts.iters, opts.warmup, page_size) != 0) {
        ret = EXIT_FAILURE;
        goto out;
    }

    if (!opts.no_chunked) {
        if (run_chunked(set_ownership, fd, map, opts.map_size, opts.mode,
                        opts.iters, opts.warmup, page_size) != 0) {
            ret = EXIT_FAILURE;
            goto out;
        }
    }

out:
    (void)set_ownership(fd, map, (char*)map + opts.map_size, PROT_NONE);
    munmap(map, opts.map_size);
    close(fd);

    return ret;
}
