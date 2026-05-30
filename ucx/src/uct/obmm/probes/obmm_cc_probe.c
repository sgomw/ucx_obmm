/*
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#  define O_CLOEXEC 0
#endif

#define OBMM_CC_PROBE_VERSION 1u
#define OBMM_CC_HELLO_MAGIC  0x4f424d4d43435031ull /* OBMMCCP1 */
#define OBMM_CC_MSG_READY    1u
#define OBMM_CC_MSG_DONE     2u
#define OBMM_CC_MSG_ERROR    3u

#define OBMM_SYSFS_ROOT      "/sys/devices/obmm"
#define OBMM_SHMDEV_PREFIX   "obmm_shmdev"
#define OBMM_DEV_FMT         "/dev/obmm_shmdev%" PRIu64

typedef enum {
    MODE_OWNERSHIP,
    MODE_MMAP
} probe_mode_t;

typedef enum {
    PROT_KIND_READ,
    PROT_KIND_WRITE
} prot_kind_t;

typedef enum {
    ROLE_WRITER,
    ROLE_READER
} handoff_role_t;

typedef enum {
    FENCE_NONE,
    FENCE_SEQ_CST,
    FENCE_ARM_ISH,
    FENCE_ARM_OSH
} fence_mode_t;

typedef enum {
    OP_READ,
    OP_WRITE
} touch_op_t;

typedef struct {
    uint64_t       memid;
    uint64_t       offset;
    uint64_t       length;
    uint64_t       iters;
    probe_mode_t   mode;
    prot_kind_t    prot;
    touch_op_t     op;
    fence_mode_t   fence_mode;
    handoff_role_t role;
    const char    *listen_ep;
    const char    *connect_ep;
    bool           verify;
    bool           memid_set;
} probe_opts_t;

typedef struct {
    uint64_t acquire_ns;
    uint64_t access_ns;
    uint64_t release_ns;
    uint64_t control_ns;
    uint64_t total_ns;
    uint64_t bytes;
} probe_stats_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t role;
    uint64_t offset;
    uint64_t length;
    uint64_t iters;
    uint32_t mode;
    uint32_t fence_mode;
    uint32_t verify;
    uint32_t page_size;
} probe_hello_t;

typedef struct {
    uint32_t type;
    uint32_t status;
    uint64_t seq;
    uint64_t value;
} probe_msg_t;

typedef int (*obmm_set_ownership_fn_t)(int fd, void *start, void *end,
                                       int prot);

static volatile uint64_t probe_sink;
static void *obmm_lib_handle;
static obmm_set_ownership_fn_t obmm_set_ownership_fn;

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  obmm_cc_probe list\n"
            "  obmm_cc_probe flip --memid N [--offset B] [--length B] [--iters N]\n"
            "                    [--mode ownership|mmap] [--prot read|write]\n"
            "  obmm_cc_probe touch --memid N [--offset B] [--length B] [--iters N]\n"
            "                     [--mode ownership|mmap] [--op read|write]\n"
            "                     [--fence none|seq_cst|arm_ish|arm_osh]\n"
            "  obmm_cc_probe handoff --role writer|reader --memid N\n"
            "                       (--listen [HOST:]PORT | --connect HOST:PORT)\n"
            "                       [--offset B] [--length B] [--iters N]\n"
            "                       [--mode ownership|mmap] [--verify]\n"
            "                       [--fence none|seq_cst|arm_ish|arm_osh]\n");
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static uint64_t bswap64_u(uint64_t v)
{
    return ((v & 0x00000000000000ffull) << 56) |
           ((v & 0x000000000000ff00ull) << 40) |
           ((v & 0x0000000000ff0000ull) << 24) |
           ((v & 0x00000000ff000000ull) << 8) |
           ((v & 0x000000ff00000000ull) >> 8) |
           ((v & 0x0000ff0000000000ull) >> 24) |
           ((v & 0x00ff000000000000ull) >> 40) |
           ((v & 0xff00000000000000ull) >> 56);
}

static uint64_t hton64_u(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return bswap64_u(v);
#else
    return v;
#endif
}

static uint64_t ntoh64_u(uint64_t v)
{
    return hton64_u(v);
}

static void hello_hton(probe_hello_t *h)
{
    h->magic      = hton64_u(h->magic);
    h->version    = htonl(h->version);
    h->role       = htonl(h->role);
    h->offset     = hton64_u(h->offset);
    h->length     = hton64_u(h->length);
    h->iters      = hton64_u(h->iters);
    h->mode       = htonl(h->mode);
    h->fence_mode = htonl(h->fence_mode);
    h->verify     = htonl(h->verify);
    h->page_size  = htonl(h->page_size);
}

static void hello_ntoh(probe_hello_t *h)
{
    h->magic      = ntoh64_u(h->magic);
    h->version    = ntohl(h->version);
    h->role       = ntohl(h->role);
    h->offset     = ntoh64_u(h->offset);
    h->length     = ntoh64_u(h->length);
    h->iters      = ntoh64_u(h->iters);
    h->mode       = ntohl(h->mode);
    h->fence_mode = ntohl(h->fence_mode);
    h->verify     = ntohl(h->verify);
    h->page_size  = ntohl(h->page_size);
}

static void msg_hton(probe_msg_t *m)
{
    m->type   = htonl(m->type);
    m->status = htonl(m->status);
    m->seq    = hton64_u(m->seq);
    m->value  = hton64_u(m->value);
}

static void msg_ntoh(probe_msg_t *m)
{
    m->type   = ntohl(m->type);
    m->status = ntohl(m->status);
    m->seq    = ntoh64_u(m->seq);
    m->value  = ntoh64_u(m->value);
}

static int parse_size_arg(const char *text, uint64_t *value)
{
    char *end = NULL;
    uint64_t scale = 1;
    unsigned long long base;

    errno = 0;
    base = strtoull(text, &end, 0);
    if ((errno != 0) || (end == text)) {
        return -1;
    }

    if (*end != '\0') {
        if ((end[1] != '\0') || (base > (ULLONG_MAX >> 30))) {
            return -1;
        }

        switch (tolower((unsigned char)*end)) {
        case 'k':
            scale = 1024ull;
            break;
        case 'm':
            scale = 1024ull * 1024ull;
            break;
        case 'g':
            scale = 1024ull * 1024ull * 1024ull;
            break;
        default:
            return -1;
        }
    }

    if (base > (ULLONG_MAX / scale)) {
        return -1;
    }

    *value = (uint64_t)(base * scale);
    return 0;
}

static int parse_u64_arg(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long v;

    errno = 0;
    v = strtoull(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0')) {
        return -1;
    }

    *value = (uint64_t)v;
    return 0;
}

static const char *mode_name(probe_mode_t mode)
{
    return (mode == MODE_OWNERSHIP) ? "ownership" : "mmap";
}

static const char *fence_name(fence_mode_t mode)
{
    switch (mode) {
    case FENCE_NONE:
        return "none";
    case FENCE_SEQ_CST:
        return "seq_cst";
    case FENCE_ARM_ISH:
        return "arm_ish";
    case FENCE_ARM_OSH:
        return "arm_osh";
    }

    return "unknown";
}

static int parse_mode(const char *text, probe_mode_t *mode)
{
    if (!strcmp(text, "ownership")) {
        *mode = MODE_OWNERSHIP;
        return 0;
    }
    if (!strcmp(text, "mmap")) {
        *mode = MODE_MMAP;
        return 0;
    }
    return -1;
}

static int parse_prot(const char *text, prot_kind_t *prot)
{
    if (!strcmp(text, "read")) {
        *prot = PROT_KIND_READ;
        return 0;
    }
    if (!strcmp(text, "write")) {
        *prot = PROT_KIND_WRITE;
        return 0;
    }
    return -1;
}

static int parse_op(const char *text, touch_op_t *op)
{
    if (!strcmp(text, "read")) {
        *op = OP_READ;
        return 0;
    }
    if (!strcmp(text, "write")) {
        *op = OP_WRITE;
        return 0;
    }
    return -1;
}

static int parse_role(const char *text, handoff_role_t *role)
{
    if (!strcmp(text, "writer")) {
        *role = ROLE_WRITER;
        return 0;
    }
    if (!strcmp(text, "reader")) {
        *role = ROLE_READER;
        return 0;
    }
    return -1;
}

static int parse_fence(const char *text, fence_mode_t *mode)
{
    if (!strcmp(text, "none")) {
        *mode = FENCE_NONE;
        return 0;
    }
    if (!strcmp(text, "seq_cst")) {
        *mode = FENCE_SEQ_CST;
        return 0;
    }
    if (!strcmp(text, "arm_ish")) {
        *mode = FENCE_ARM_ISH;
        return 0;
    }
    if (!strcmp(text, "arm_osh")) {
        *mode = FENCE_ARM_OSH;
        return 0;
    }
    return -1;
}

static void init_opts(probe_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->length     = 4096;
    opts->iters      = 1000;
    opts->mode       = MODE_OWNERSHIP;
    opts->prot       = PROT_KIND_WRITE;
    opts->op         = OP_WRITE;
    opts->fence_mode = FENCE_SEQ_CST;
    opts->role       = ROLE_WRITER;
}

static int need_arg(int argc, char **argv, int *i)
{
    if ((*i + 1) >= argc) {
        fprintf(stderr, "missing argument for %s\n", argv[*i]);
        return -1;
    }
    ++(*i);
    return 0;
}

static int parse_common_opt(int argc, char **argv, int *i, probe_opts_t *opts)
{
    const char *arg = argv[*i];

    if (!strcmp(arg, "--memid")) {
        if (need_arg(argc, argv, i) != 0) {
            return -1;
        }
        if ((parse_u64_arg(argv[*i], &opts->memid) != 0) ||
            (opts->memid == 0)) {
            fprintf(stderr, "invalid memid: %s\n", argv[*i]);
            return -1;
        }
        opts->memid_set = true;
        return 1;
    }

    if (!strcmp(arg, "--offset")) {
        if (need_arg(argc, argv, i) != 0) {
            return -1;
        }
        if (parse_size_arg(argv[*i], &opts->offset) != 0) {
            fprintf(stderr, "invalid offset: %s\n", argv[*i]);
            return -1;
        }
        return 1;
    }

    if (!strcmp(arg, "--length")) {
        if (need_arg(argc, argv, i) != 0) {
            return -1;
        }
        if ((parse_size_arg(argv[*i], &opts->length) != 0) ||
            (opts->length == 0)) {
            fprintf(stderr, "invalid length: %s\n", argv[*i]);
            return -1;
        }
        return 1;
    }

    if (!strcmp(arg, "--iters")) {
        if (need_arg(argc, argv, i) != 0) {
            return -1;
        }
        if ((parse_u64_arg(argv[*i], &opts->iters) != 0) ||
            (opts->iters == 0)) {
            fprintf(stderr, "invalid iters: %s\n", argv[*i]);
            return -1;
        }
        return 1;
    }

    if (!strcmp(arg, "--mode")) {
        if (need_arg(argc, argv, i) != 0) {
            return -1;
        }
        if (parse_mode(argv[*i], &opts->mode) != 0) {
            fprintf(stderr, "invalid mode: %s\n", argv[*i]);
            return -1;
        }
        return 1;
    }

    if (!strcmp(arg, "--fence")) {
        if (need_arg(argc, argv, i) != 0) {
            return -1;
        }
        if (parse_fence(argv[*i], &opts->fence_mode) != 0) {
            fprintf(stderr, "invalid fence: %s\n", argv[*i]);
            return -1;
        }
        return 1;
    }

    return 0;
}

static int parse_args(int argc, char **argv, const char *cmd, probe_opts_t *opts)
{
    int i;

    for (i = 2; i < argc; ++i) {
        int common = parse_common_opt(argc, argv, &i, opts);
        if (common < 0) {
            return -1;
        }
        if (common > 0) {
            continue;
        }

        if (!strcmp(cmd, "flip") && !strcmp(argv[i], "--prot")) {
            if (need_arg(argc, argv, &i) != 0) {
                return -1;
            }
            if (parse_prot(argv[i], &opts->prot) != 0) {
                fprintf(stderr, "invalid prot: %s\n", argv[i]);
                return -1;
            }
            continue;
        }

        if (!strcmp(cmd, "touch") && !strcmp(argv[i], "--op")) {
            if (need_arg(argc, argv, &i) != 0) {
                return -1;
            }
            if (parse_op(argv[i], &opts->op) != 0) {
                fprintf(stderr, "invalid op: %s\n", argv[i]);
                return -1;
            }
            opts->prot = (opts->op == OP_READ) ? PROT_KIND_READ :
                                                PROT_KIND_WRITE;
            continue;
        }

        if (!strcmp(cmd, "handoff") && !strcmp(argv[i], "--role")) {
            if (need_arg(argc, argv, &i) != 0) {
                return -1;
            }
            if (parse_role(argv[i], &opts->role) != 0) {
                fprintf(stderr, "invalid role: %s\n", argv[i]);
                return -1;
            }
            continue;
        }

        if (!strcmp(cmd, "handoff") && !strcmp(argv[i], "--listen")) {
            if (need_arg(argc, argv, &i) != 0) {
                return -1;
            }
            opts->listen_ep = argv[i];
            continue;
        }

        if (!strcmp(cmd, "handoff") && !strcmp(argv[i], "--connect")) {
            if (need_arg(argc, argv, &i) != 0) {
                return -1;
            }
            opts->connect_ep = argv[i];
            continue;
        }

        if (!strcmp(cmd, "handoff") && !strcmp(argv[i], "--verify")) {
            opts->verify = true;
            continue;
        }

        fprintf(stderr, "unknown option for %s: %s\n", cmd, argv[i]);
        return -1;
    }

    if (strcmp(cmd, "list") && !opts->memid_set) {
        fprintf(stderr, "%s requires --memid\n", cmd);
        return -1;
    }

    if (!strcmp(cmd, "handoff")) {
        if ((opts->listen_ep == NULL) == (opts->connect_ep == NULL)) {
            fprintf(stderr, "handoff requires exactly one of --listen or --connect\n");
            return -1;
        }
    }

    return 0;
}

static size_t page_size_or_die(void)
{
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        exit(EXIT_FAILURE);
    }

    return (size_t)page_size;
}

static int validate_page_range(const probe_opts_t *opts)
{
    size_t page_size = page_size_or_die();

    if ((opts->offset % page_size) != 0) {
        fprintf(stderr, "offset must be page aligned: offset=%" PRIu64
                " page_size=%zu\n", opts->offset, page_size);
        return -1;
    }

    if ((opts->length % page_size) != 0) {
        fprintf(stderr, "length must be page aligned: length=%" PRIu64
                " page_size=%zu\n", opts->length, page_size);
        return -1;
    }

    if (opts->length > SIZE_MAX) {
        fprintf(stderr, "length is too large for this process\n");
        return -1;
    }

    if (opts->offset > (uint64_t)LLONG_MAX) {
        fprintf(stderr, "offset is too large for mmap off_t: %" PRIu64 "\n",
                opts->offset);
        return -1;
    }

    return 0;
}

static int open_memdev(uint64_t memid)
{
    char path[128];
    int ret;
    int fd;

    ret = snprintf(path, sizeof(path), OBMM_DEV_FMT, memid);
    if ((ret < 0) || (ret >= (int)sizeof(path))) {
        fprintf(stderr, "memdev path too long for memid=%" PRIu64 "\n", memid);
        return -1;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s, O_RDWR) failed: %s\n", path, strerror(errno));
        return -1;
    }

    return fd;
}

static int prot_from_kind(prot_kind_t prot)
{
    return (prot == PROT_KIND_READ) ? PROT_READ : PROT_WRITE;
}

static const char *prot_name(prot_kind_t prot)
{
    return (prot == PROT_KIND_READ) ? "read" : "write";
}

static void *map_region(int fd, const probe_opts_t *opts, int prot)
{
    void *ptr = mmap(NULL, (size_t)opts->length, prot, MAP_SHARED, fd,
                     (off_t)opts->offset);

    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap(offset=%" PRIu64 ", length=%" PRIu64
                ", prot=0x%x) failed: %s\n",
                opts->offset, opts->length, prot, strerror(errno));
        return NULL;
    }

    return ptr;
}

static int unmap_region(void *ptr, uint64_t length)
{
    if (munmap(ptr, (size_t)length) != 0) {
        fprintf(stderr, "munmap(%p, %" PRIu64 ") failed: %s\n", ptr, length,
                strerror(errno));
        return -1;
    }

    return 0;
}

static int set_ownership_range(int fd, void *ptr, uint64_t length, int prot)
{
    char *end = (char*)ptr + length;

    if (obmm_set_ownership_fn == NULL) {
        const char *env_path = getenv("OBMM_LIBOBMM_PATH");
        const char *candidates[3];
        unsigned i;

        candidates[0] = env_path;
        candidates[1] = "libobmm.so";
        candidates[2] = "libobmm.so.0";

        for (i = 0; i < 3; ++i) {
            if ((candidates[i] == NULL) || (candidates[i][0] == '\0')) {
                continue;
            }

            obmm_lib_handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
            if (obmm_lib_handle != NULL) {
                break;
            }
        }

        if (obmm_lib_handle == NULL) {
            fprintf(stderr, "failed to load libobmm.so; set "
                    "OBMM_LIBOBMM_PATH=/path/to/libobmm.so if it is not in "
                    "the runtime loader path: %s\n", dlerror());
            return -1;
        }

        dlerror();
        obmm_set_ownership_fn =
            (obmm_set_ownership_fn_t)dlsym(obmm_lib_handle,
                                           "obmm_set_ownership");
        if (obmm_set_ownership_fn == NULL) {
            fprintf(stderr, "failed to resolve obmm_set_ownership: %s\n",
                    dlerror());
            return -1;
        }
    }

    if (obmm_set_ownership_fn(fd, ptr, end, prot) != 0) {
        fprintf(stderr, "obmm_set_ownership(%p, %p, prot=0x%x) failed: %s\n",
                ptr, end, prot, strerror(errno));
        return -1;
    }

    return 0;
}

static int apply_fence(fence_mode_t mode)
{
    switch (mode) {
    case FENCE_NONE:
        return 0;
    case FENCE_SEQ_CST:
        atomic_thread_fence(memory_order_seq_cst);
        return 0;
    case FENCE_ARM_ISH:
#if defined(__aarch64__)
        __asm__ __volatile__("dmb ish" ::: "memory");
        return 0;
#else
        fprintf(stderr, "--fence arm_ish requires aarch64\n");
        return -1;
#endif
    case FENCE_ARM_OSH:
#if defined(__aarch64__)
        __asm__ __volatile__("dmb osh" ::: "memory");
        return 0;
#else
        fprintf(stderr, "--fence arm_osh requires aarch64\n");
        return -1;
#endif
    }

    return -1;
}

static int validate_fence_supported(fence_mode_t mode)
{
#if !defined(__aarch64__)
    if ((mode == FENCE_ARM_ISH) || (mode == FENCE_ARM_OSH)) {
        fprintf(stderr, "--fence %s requires aarch64\n", fence_name(mode));
        return -1;
    }
#else
    (void)mode;
#endif
    return 0;
}

static uint64_t pattern_word(uint64_t seq, uint64_t index)
{
    return 0x9e3779b97f4a7c15ull ^ (seq * 0xd6e8feb86659fd93ull) ^
           (index * 0xa5a5a5a55a5a5a5aull);
}

static uint64_t fill_pattern(void *ptr, size_t length, uint64_t seq)
{
    uint64_t *words = (uint64_t*)ptr;
    size_t nwords = length / sizeof(*words);
    uint8_t *tail = (uint8_t*)(words + nwords);
    uint64_t sum = 0;
    size_t i;

    for (i = 0; i < nwords; ++i) {
        uint64_t v = pattern_word(seq, i);
        words[i] = v;
        sum += v;
    }

    for (i = 0; i < (length % sizeof(*words)); ++i) {
        uint8_t v = (uint8_t)pattern_word(seq, nwords + i);
        tail[i] = v;
        sum += v;
    }

    return sum;
}

static uint64_t read_sum(const void *ptr, size_t length)
{
    const volatile uint64_t *words = (const volatile uint64_t*)ptr;
    size_t nwords = length / sizeof(*words);
    const volatile uint8_t *tail =
        (const volatile uint8_t*)((const uint64_t*)ptr + nwords);
    uint64_t sum = 0;
    size_t i;

    for (i = 0; i < nwords; ++i) {
        sum += words[i];
    }

    for (i = 0; i < (length % sizeof(uint64_t)); ++i) {
        sum += tail[i];
    }

    probe_sink = sum;
    return sum;
}

static void print_stats(const char *name, const probe_opts_t *opts,
                        const probe_stats_t *stats)
{
    double iters = (double)opts->iters;
    double total_s = (double)stats->total_ns / 1.0e9;
    double access_s = (double)stats->access_ns / 1.0e9;
    double total_gib = (double)stats->bytes / (1024.0 * 1024.0 * 1024.0);

    printf("%s\n", name);
    printf("  mode            : %s\n", mode_name(opts->mode));
    printf("  length          : %" PRIu64 "\n", opts->length);
    printf("  iters           : %" PRIu64 "\n", opts->iters);
    printf("  fence           : %s\n", fence_name(opts->fence_mode));
    printf("  acquire ns/op   : %.2f\n", stats->acquire_ns / iters);
    printf("  access ns/op    : %.2f\n", stats->access_ns / iters);
    printf("  release ns/op   : %.2f\n", stats->release_ns / iters);
    if (stats->control_ns != 0) {
        printf("  control ns/op   : %.2f\n", stats->control_ns / iters);
    }
    printf("  total ns/op     : %.2f\n", stats->total_ns / iters);
    if ((stats->bytes != 0) && (access_s > 0.0)) {
        printf("  access GiB/s    : %.3f\n", total_gib / access_s);
    }
    if ((stats->bytes != 0) && (total_s > 0.0)) {
        printf("  end-to-end GiB/s: %.3f\n", total_gib / total_s);
    }
}

static int run_flip(const probe_opts_t *opts)
{
    probe_stats_t stats = {0};
    uint64_t i;
    int fd;
    int prot = prot_from_kind(opts->prot);
    uint64_t total_start;
    uint64_t total_end;

    if (validate_page_range(opts) != 0) {
        return EXIT_FAILURE;
    }

    fd = open_memdev(opts->memid);
    if (fd < 0) {
        return EXIT_FAILURE;
    }

    total_start = now_ns();

    if (opts->mode == MODE_OWNERSHIP) {
        void *ptr = map_region(fd, opts, PROT_NONE);
        if (ptr == NULL) {
            close(fd);
            return EXIT_FAILURE;
        }

        for (i = 0; i < opts->iters; ++i) {
            uint64_t t0 = now_ns();
            if (set_ownership_range(fd, ptr, opts->length, prot) != 0) {
                unmap_region(ptr, opts->length);
                close(fd);
                return EXIT_FAILURE;
            }
            uint64_t t1 = now_ns();
            if (set_ownership_range(fd, ptr, opts->length, PROT_NONE) != 0) {
                unmap_region(ptr, opts->length);
                close(fd);
                return EXIT_FAILURE;
            }
            uint64_t t2 = now_ns();

            stats.acquire_ns += t1 - t0;
            stats.release_ns += t2 - t1;
        }

        if (unmap_region(ptr, opts->length) != 0) {
            close(fd);
            return EXIT_FAILURE;
        }
    } else {
        for (i = 0; i < opts->iters; ++i) {
            uint64_t t0 = now_ns();
            void *ptr = map_region(fd, opts, prot);
            uint64_t t1 = now_ns();
            if (ptr == NULL) {
                close(fd);
                return EXIT_FAILURE;
            }
            if (unmap_region(ptr, opts->length) != 0) {
                close(fd);
                return EXIT_FAILURE;
            }
            uint64_t t2 = now_ns();

            stats.acquire_ns += t1 - t0;
            stats.release_ns += t2 - t1;
        }
    }

    total_end = now_ns();
    stats.total_ns = total_end - total_start;

    printf("flip prot=%s\n", prot_name(opts->prot));
    print_stats("flip results", opts, &stats);

    close(fd);
    return EXIT_SUCCESS;
}

static int touch_once(int fd, const probe_opts_t *opts, void **persistent_ptr,
                      probe_stats_t *stats, uint64_t seq)
{
    void *ptr = NULL;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    int prot = (opts->op == OP_READ) ? PROT_READ : PROT_WRITE;

    t0 = now_ns();
    if (opts->mode == MODE_OWNERSHIP) {
        ptr = *persistent_ptr;
        if (set_ownership_range(fd, ptr, opts->length, prot) != 0) {
            return -1;
        }
    } else {
        ptr = map_region(fd, opts, prot);
        if (ptr == NULL) {
            return -1;
        }
    }
    t1 = now_ns();

    if (opts->op == OP_READ) {
        read_sum(ptr, (size_t)opts->length);
    } else {
        (void)fill_pattern(ptr, (size_t)opts->length, seq);
        if (apply_fence(opts->fence_mode) != 0) {
            return -1;
        }
    }
    t2 = now_ns();

    if (opts->mode == MODE_OWNERSHIP) {
        if (set_ownership_range(fd, ptr, opts->length, PROT_NONE) != 0) {
            return -1;
        }
    } else {
        if (unmap_region(ptr, opts->length) != 0) {
            return -1;
        }
    }
    t3 = now_ns();

    stats->acquire_ns += t1 - t0;
    stats->access_ns += t2 - t1;
    stats->release_ns += t3 - t2;
    stats->bytes += opts->length;
    return 0;
}

static int run_touch(const probe_opts_t *opts)
{
    probe_stats_t stats = {0};
    uint64_t total_start;
    uint64_t total_end;
    uint64_t i;
    void *ptr = NULL;
    int fd;

    if (validate_page_range(opts) != 0) {
        return EXIT_FAILURE;
    }
    if (validate_fence_supported(opts->fence_mode) != 0) {
        return EXIT_FAILURE;
    }

    fd = open_memdev(opts->memid);
    if (fd < 0) {
        return EXIT_FAILURE;
    }

    if (opts->mode == MODE_OWNERSHIP) {
        ptr = map_region(fd, opts, PROT_NONE);
        if (ptr == NULL) {
            close(fd);
            return EXIT_FAILURE;
        }
    }

    total_start = now_ns();
    for (i = 0; i < opts->iters; ++i) {
        if (touch_once(fd, opts, &ptr, &stats, i) != 0) {
            if (ptr != NULL) {
                unmap_region(ptr, opts->length);
            }
            close(fd);
            return EXIT_FAILURE;
        }
    }
    total_end = now_ns();
    stats.total_ns = total_end - total_start;

    if ((opts->mode == MODE_OWNERSHIP) && (ptr != NULL)) {
        if (unmap_region(ptr, opts->length) != 0) {
            close(fd);
            return EXIT_FAILURE;
        }
    }

    printf("touch op=%s\n", (opts->op == OP_READ) ? "read" : "write");
    print_stats("touch results", opts, &stats);

    close(fd);
    return EXIT_SUCCESS;
}

static int split_endpoint(const char *ep, char *host, size_t host_size,
                          char *port, size_t port_size, bool listen)
{
    const char *colon = strrchr(ep, ':');

    if (colon == NULL) {
        if (!listen) {
            fprintf(stderr, "connect endpoint must be HOST:PORT: %s\n", ep);
            return -1;
        }
        host[0] = '\0';
        snprintf(port, port_size, "%s", ep);
        return 0;
    }

    if ((size_t)(colon - ep) >= host_size) {
        fprintf(stderr, "endpoint host is too long: %s\n", ep);
        return -1;
    }

    memcpy(host, ep, (size_t)(colon - ep));
    host[colon - ep] = '\0';
    snprintf(port, port_size, "%s", colon + 1);
    return 0;
}

static int connect_socket(const char *endpoint)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char host[256];
    char port[32];
    int fd = -1;
    int ret;

    if (split_endpoint(endpoint, host, sizeof(host), port, sizeof(port),
                       false) != 0) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    ret = getaddrinfo(host, port, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo(%s) failed: %s\n", endpoint,
                gai_strerror(ret));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    if (fd < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n", endpoint, strerror(errno));
    }

    freeaddrinfo(res);
    return fd;
}

static int listen_socket(const char *endpoint)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char host[256];
    char port[32];
    int listen_fd = -1;
    int fd = -1;
    int ret;
    int one = 1;

    if (split_endpoint(endpoint, host, sizeof(host), port, sizeof(port),
                       true) != 0) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;

    ret = getaddrinfo((host[0] == '\0') ? NULL : host, port, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo(%s) failed: %s\n", endpoint,
                gai_strerror(ret));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        listen_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }

        (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                         sizeof(one));

        if ((bind(listen_fd, ai->ai_addr, ai->ai_addrlen) == 0) &&
            (listen(listen_fd, 1) == 0)) {
            break;
        }

        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(res);

    if (listen_fd < 0) {
        fprintf(stderr, "listen(%s) failed: %s\n", endpoint, strerror(errno));
        return -1;
    }

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        fprintf(stderr, "accept(%s) failed: %s\n", endpoint, strerror(errno));
    }

    close(listen_fd);
    return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *ptr = (const char*)buf;

    while (len > 0) {
        ssize_t ret = send(fd, ptr, len, MSG_NOSIGNAL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "send failed: %s\n", strerror(errno));
            return -1;
        }
        if (ret == 0) {
            fprintf(stderr, "send returned 0\n");
            return -1;
        }
        ptr += ret;
        len -= (size_t)ret;
    }

    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *ptr = (char*)buf;

    while (len > 0) {
        ssize_t ret = recv(fd, ptr, len, 0);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "recv failed: %s\n", strerror(errno));
            return -1;
        }
        if (ret == 0) {
            fprintf(stderr, "peer closed connection\n");
            return -1;
        }
        ptr += ret;
        len -= (size_t)ret;
    }

    return 0;
}

static int send_msg(int fd, uint32_t type, uint32_t status, uint64_t seq,
                    uint64_t value)
{
    probe_msg_t msg = {
        .type = type,
        .status = status,
        .seq = seq,
        .value = value
    };

    msg_hton(&msg);
    return send_all(fd, &msg, sizeof(msg));
}

static int recv_msg(int fd, probe_msg_t *msg)
{
    if (recv_all(fd, msg, sizeof(*msg)) != 0) {
        return -1;
    }
    msg_ntoh(msg);
    return 0;
}

static int exchange_hello(int sock, const probe_opts_t *opts)
{
    probe_hello_t local = {
        .magic      = OBMM_CC_HELLO_MAGIC,
        .version    = OBMM_CC_PROBE_VERSION,
        .role       = (uint32_t)opts->role,
        .offset     = opts->offset,
        .length     = opts->length,
        .iters      = opts->iters,
        .mode       = (uint32_t)opts->mode,
        .fence_mode = (uint32_t)opts->fence_mode,
        .verify     = opts->verify ? 1u : 0u,
        .page_size  = (uint32_t)page_size_or_die()
    };
    probe_hello_t outbound = local;
    probe_hello_t remote;

    hello_hton(&outbound);
    if (send_all(sock, &outbound, sizeof(outbound)) != 0) {
        return -1;
    }
    if (recv_all(sock, &remote, sizeof(remote)) != 0) {
        return -1;
    }
    hello_ntoh(&remote);

    if (remote.magic != OBMM_CC_HELLO_MAGIC) {
        fprintf(stderr, "peer sent invalid magic\n");
        return -1;
    }
    if (remote.version != OBMM_CC_PROBE_VERSION) {
        fprintf(stderr, "version mismatch: local=%u remote=%u\n",
                OBMM_CC_PROBE_VERSION, remote.version);
        return -1;
    }
    if (remote.role == local.role) {
        fprintf(stderr, "role mismatch: both peers use role=%u\n", local.role);
        return -1;
    }
    if ((remote.offset != local.offset) || (remote.length != local.length) ||
        (remote.iters != local.iters) || (remote.mode != local.mode) ||
        (remote.fence_mode != local.fence_mode) ||
        (remote.verify != local.verify) ||
        (remote.page_size != local.page_size)) {
        fprintf(stderr, "handoff configuration mismatch with peer\n");
        return -1;
    }

    return 0;
}

static int run_handoff_writer(int fd, int sock, const probe_opts_t *opts,
                              void *persistent_ptr, probe_stats_t *stats)
{
    uint64_t i;
    uint64_t total_start = now_ns();

    for (i = 0; i < opts->iters; ++i) {
        probe_msg_t done;
        uint64_t checksum;
        uint64_t t0;
        uint64_t t1;
        uint64_t t2;
        uint64_t t3;
        uint64_t t4;
        void *ptr;

        t0 = now_ns();
        if (opts->mode == MODE_OWNERSHIP) {
            ptr = persistent_ptr;
            if (set_ownership_range(fd, ptr, opts->length, PROT_WRITE) != 0) {
                return -1;
            }
        } else {
            ptr = map_region(fd, opts, PROT_WRITE);
            if (ptr == NULL) {
                return -1;
            }
        }
        t1 = now_ns();

        checksum = fill_pattern(ptr, (size_t)opts->length, i);
        if (apply_fence(opts->fence_mode) != 0) {
            return -1;
        }
        t2 = now_ns();

        if (opts->mode == MODE_OWNERSHIP) {
            if (set_ownership_range(fd, ptr, opts->length, PROT_NONE) != 0) {
                return -1;
            }
        } else {
            if (unmap_region(ptr, opts->length) != 0) {
                return -1;
            }
        }
        t3 = now_ns();

        if (send_msg(sock, OBMM_CC_MSG_READY, 0, i,
                     opts->verify ? checksum : 0) != 0) {
            return -1;
        }
        if (recv_msg(sock, &done) != 0) {
            return -1;
        }
        t4 = now_ns();

        if ((done.type != OBMM_CC_MSG_DONE) || (done.seq != i) ||
            (done.status != 0)) {
            fprintf(stderr, "reader reported failure at seq=%" PRIu64
                    " status=%u type=%u\n", i, done.status, done.type);
            return -1;
        }

        stats->acquire_ns += t1 - t0;
        stats->access_ns += t2 - t1;
        stats->release_ns += t3 - t2;
        stats->control_ns += t4 - t3;
        stats->bytes += opts->length;
    }

    stats->total_ns = now_ns() - total_start;
    return 0;
}

static int run_handoff_reader(int fd, int sock, const probe_opts_t *opts,
                              void *persistent_ptr, probe_stats_t *stats)
{
    uint64_t i;
    uint64_t total_start = now_ns();

    for (i = 0; i < opts->iters; ++i) {
        probe_msg_t ready;
        uint64_t checksum;
        uint64_t t0;
        uint64_t t1;
        uint64_t t2;
        uint64_t t3;
        void *ptr;
        uint32_t status = 0;

        if (recv_msg(sock, &ready) != 0) {
            return -1;
        }
        if ((ready.type != OBMM_CC_MSG_READY) || (ready.seq != i)) {
            fprintf(stderr, "writer sent unexpected message at seq=%" PRIu64
                    " type=%u msg_seq=%" PRIu64 "\n",
                    i, ready.type, ready.seq);
            return -1;
        }

        t0 = now_ns();
        if (opts->mode == MODE_OWNERSHIP) {
            ptr = persistent_ptr;
            if (set_ownership_range(fd, ptr, opts->length, PROT_READ) != 0) {
                return -1;
            }
        } else {
            ptr = map_region(fd, opts, PROT_READ);
            if (ptr == NULL) {
                return -1;
            }
        }
        t1 = now_ns();

        checksum = read_sum(ptr, (size_t)opts->length);
        t2 = now_ns();

        if (opts->verify && (checksum != ready.value)) {
            fprintf(stderr, "checksum mismatch at seq=%" PRIu64
                    ": expected=0x%016" PRIx64 " actual=0x%016" PRIx64 "\n",
                    i, ready.value, checksum);
            status = 1;
        }

        if (opts->mode == MODE_OWNERSHIP) {
            if (set_ownership_range(fd, ptr, opts->length, PROT_NONE) != 0) {
                return -1;
            }
        } else {
            if (unmap_region(ptr, opts->length) != 0) {
                return -1;
            }
        }
        t3 = now_ns();

        if (send_msg(sock, OBMM_CC_MSG_DONE, status, i, 0) != 0) {
            return -1;
        }
        if (status != 0) {
            return -1;
        }

        stats->acquire_ns += t1 - t0;
        stats->access_ns += t2 - t1;
        stats->release_ns += t3 - t2;
        stats->bytes += opts->length;
    }

    stats->total_ns = now_ns() - total_start;
    return 0;
}

static int run_handoff(const probe_opts_t *opts)
{
    probe_stats_t stats = {0};
    void *ptr = NULL;
    int fd;
    int sock;
    int ret;

    if (validate_page_range(opts) != 0) {
        return EXIT_FAILURE;
    }
    if (validate_fence_supported(opts->fence_mode) != 0) {
        return EXIT_FAILURE;
    }

    if (opts->connect_ep != NULL) {
        sock = connect_socket(opts->connect_ep);
    } else {
        sock = listen_socket(opts->listen_ep);
    }
    if (sock < 0) {
        return EXIT_FAILURE;
    }

    if (exchange_hello(sock, opts) != 0) {
        close(sock);
        return EXIT_FAILURE;
    }

    fd = open_memdev(opts->memid);
    if (fd < 0) {
        close(sock);
        return EXIT_FAILURE;
    }

    if (opts->mode == MODE_OWNERSHIP) {
        ptr = map_region(fd, opts, PROT_NONE);
        if (ptr == NULL) {
            close(fd);
            close(sock);
            return EXIT_FAILURE;
        }
    }

    if (opts->role == ROLE_WRITER) {
        ret = run_handoff_writer(fd, sock, opts, ptr, &stats);
    } else {
        ret = run_handoff_reader(fd, sock, opts, ptr, &stats);
    }

    if ((opts->mode == MODE_OWNERSHIP) && (ptr != NULL)) {
        if (unmap_region(ptr, opts->length) != 0) {
            ret = -1;
        }
    }

    close(fd);
    close(sock);

    if (ret != 0) {
        return EXIT_FAILURE;
    }

    printf("handoff role=%s verify=%s\n",
           (opts->role == ROLE_WRITER) ? "writer" : "reader",
           opts->verify ? "yes" : "no");
    print_stats("handoff results", opts, &stats);
    return EXIT_SUCCESS;
}

static int read_text_file(const char *path, char *buf, size_t len)
{
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }

    buf[n] = '\0';
    while ((n > 0) && isspace((unsigned char)buf[n - 1])) {
        buf[--n] = '\0';
    }

    return 0;
}

static int print_one_device(const char *name)
{
    const size_t prefix_len = strlen(OBMM_SHMDEV_PREFIX);
    char path[PATH_MAX];
    char type[32] = "?";
    char size[64] = "?";
    char allow[32] = "?";
    char dcna[64] = "-";
    char deid[128] = "-";
    uint64_t memid;
    const char *num;
    char *end = NULL;

    if (strncmp(name, OBMM_SHMDEV_PREFIX, prefix_len) != 0) {
        return 0;
    }

    num = name + prefix_len;
    errno = 0;
    memid = strtoull(num, &end, 10);
    if ((errno != 0) || (end == num) || (*end != '\0') || (memid == 0)) {
        return 0;
    }

    snprintf(path, sizeof(path), "%s/%s/type", OBMM_SYSFS_ROOT, name);
    (void)read_text_file(path, type, sizeof(type));

    snprintf(path, sizeof(path), "%s/%s/size", OBMM_SYSFS_ROOT, name);
    (void)read_text_file(path, size, sizeof(size));

    snprintf(path, sizeof(path), "%s/%s/allow_mmap", OBMM_SYSFS_ROOT, name);
    (void)read_text_file(path, allow, sizeof(allow));

    if (!strcmp(type, "import")) {
        snprintf(path, sizeof(path), "%s/%s/import_info/dcna",
                 OBMM_SYSFS_ROOT, name);
        (void)read_text_file(path, dcna, sizeof(dcna));
        snprintf(path, sizeof(path), "%s/%s/import_info/deid",
                 OBMM_SYSFS_ROOT, name);
        (void)read_text_file(path, deid, sizeof(deid));
    } else if (!strcmp(type, "export")) {
        snprintf(path, sizeof(path), "%s/%s/export_info/deid",
                 OBMM_SYSFS_ROOT, name);
        (void)read_text_file(path, deid, sizeof(deid));
    }

    printf("%-18s memid=%" PRIu64 " type=%-6s size=%-12s allow_mmap=%s"
           " dcna=%s deid=%s\n",
           name, memid, type, size, allow, dcna, deid);
    return 0;
}

static int run_list(void)
{
    DIR *dir = opendir(OBMM_SYSFS_ROOT);
    struct dirent *entry;

    if (dir == NULL) {
        fprintf(stderr, "opendir(%s) failed: %s\n", OBMM_SYSFS_ROOT,
                strerror(errno));
        return EXIT_FAILURE;
    }

    while ((entry = readdir(dir)) != NULL) {
        (void)print_one_device(entry->d_name);
    }

    closedir(dir);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    probe_opts_t opts;
    const char *cmd;

    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    cmd = argv[1];
    init_opts(&opts);

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage(stdout);
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "list") && strcmp(cmd, "flip") && strcmp(cmd, "touch") &&
        strcmp(cmd, "handoff")) {
        fprintf(stderr, "unknown command: %s\n", cmd);
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (parse_args(argc, argv, cmd, &opts) != 0) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (!strcmp(cmd, "list")) {
        return run_list();
    }
    if (!strcmp(cmd, "flip")) {
        return run_flip(&opts);
    }
    if (!strcmp(cmd, "touch")) {
        return run_touch(&opts);
    }

    return run_handoff(&opts);
}
