/*
 * Standalone two-node cacheable-CC batch path probe for obmm.
 *
 * This probe models a receiver-owned bulk epoch:
 *
 *   A owns B's imported/exported CC range for WRITE once, copies N messages
 *   into contiguous receiver-owned slots, then drops the whole range to NONE.
 *   B owns the matching local/exported CC range for READ once, copies/touches
 *   N messages, then drops the whole range to NONE.
 *
 * TCP is used only for synchronization and timing exchange. No payload bytes
 * are sent over TCP.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_SIZES       16
#define MAX_BATCHES     16
#define NSEC_PER_USEC   1000.0
#define CTRL_MAGIC      0x4f424d4d43434231ull /* OBMMCCB1 */
#define CTRL_VERSION    1u

typedef int (*obmm_set_ownership_fn_t)(int fd, void *start, void *end,
                                       int prot);

typedef enum {
    ROLE_UNSET,
    ROLE_A,
    ROLE_B
} role_t;

typedef enum {
    CMD_READ_BATCH = 1,
    CMD_STOP       = 2
} cmd_type_t;

typedef struct {
    uint64_t acq;
    uint64_t copy;
    uint64_t release;
    uint64_t total;
    uint64_t checksum;
} sample_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t type;
    uint64_t seq;
    uint64_t offset;
    uint64_t own_len;
    uint64_t msg_stride;
    uint64_t total_len;
    uint32_t batch_count;
    uint32_t reserved;
} ctrl_cmd_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t status;
    uint64_t seq;
    sample_t sample;
} ctrl_reply_t;

typedef struct {
    char     local_dev[PATH_MAX];
    char     peer_dev[PATH_MAX];
    char     connect_ep[256];
    char     listen_ep[256];
    size_t   sizes[MAX_SIZES];
    unsigned size_count;
    unsigned batches[MAX_BATCHES];
    unsigned batch_count;
    size_t   map_size;
    size_t   offset;
    size_t   own_granule;
    size_t   header_size;
    unsigned iters;
    unsigned warmup;
    role_t   role;
    int      summary_only;
} options_t;

typedef struct {
    int    fd;
    void  *addr;
    size_t size;
    char   path[PATH_MAX];
} mapped_region_t;

typedef struct {
    sample_t writer;
    sample_t reader;
    uint64_t path_total;
    uint64_t wall;
} iter_result_t;

typedef struct {
    double writer_acq;
    double writer_copy;
    double writer_release;
    double writer_total;
    double reader_acq;
    double reader_copy;
    double reader_release;
    double reader_total;
    double path_total;
    double wall;
} median_result_t;

static volatile uint64_t g_checksum;

static uint64_t now_nsec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static double usec(uint64_t nsec)
{
    return (double)nsec / NSEC_PER_USEC;
}

static int is_power_of_two(size_t value)
{
    return (value != 0) && ((value & (value - 1u)) == 0);
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
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
    return values[count / 2u];
}

static int parse_size(const char *str, size_t *value_p)
{
    char               *end;
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

    if ((*end != '\0') || (value > (ULLONG_MAX / scale))) {
        return -1;
    }

    *value_p = (size_t)(value * scale);
    return 0;
}

static void set_dev_from_memid(char *path, size_t path_len,
                               const char *memid_str)
{
    uint64_t memid;
    char     *end;

    errno = 0;
    memid = strtoull(memid_str, &end, 0);
    if ((errno != 0) || (end == memid_str) || (*end != '\0') ||
        (memid == 0)) {
        fprintf(stderr, "invalid memid: %s\n", memid_str);
        exit(EXIT_FAILURE);
    }

    snprintf(path, path_len, "/dev/obmm_shmdev%" PRIu64, memid);
}

static void options_init(options_t *opts)
{
    memset(opts, 0, sizeof(*opts));

    opts->sizes[0]    = 2u * 1024u * 1024u;
    opts->sizes[1]    = 4u * 1024u * 1024u;
    opts->size_count  = 2;
    opts->batches[0]  = 1;
    opts->batches[1]  = 2;
    opts->batches[2]  = 4;
    opts->batches[3]  = 8;
    opts->batch_count = 4;
    opts->map_size    = 128u * 1024u * 1024u;
    opts->own_granule = 2u * 1024u * 1024u;
    opts->header_size = 4u * 1024u;
    opts->iters       = 10;
    opts->warmup      = 2;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --role b --listen [HOST:]PORT [mem options] [options]\n"
            "  %s --role a --connect HOST:PORT [mem options] [options]\n"
            "\n"
            "Memory options:\n"
            "  --local-export-memid ID   this node's exported CC memid\n"
            "  --peer-import-memid ID    this node's imported peer CC memid\n"
            "  --local-dev PATH          this node's exported CC shmdev\n"
            "  --peer-dev PATH           this node's imported peer CC shmdev\n"
            "\n"
            "Role requirements:\n"
            "  role a needs --peer-import-memid/--peer-dev for B's CC region\n"
            "  role b needs --local-export-memid/--local-dev for B's CC region\n"
            "\n"
            "Options:\n"
            "  --sizes LIST              comma-separated payload sizes\n"
            "                            (default: 2M,4M)\n"
            "  --batches LIST            comma-separated batch counts\n"
            "                            (default: 1,2,4,8)\n"
            "  --iters N                 measured iterations per case "
            "(default: 10)\n"
            "  --warmup N                warmup iterations per case "
            "(default: 2)\n"
            "  --map-size SIZE           mmap length (default: 128M)\n"
            "  --offset SIZE             page-aligned test offset (default: 0)\n"
            "  --own-granule SIZE        ownership alignment (default: 2M)\n"
            "  --header-size SIZE        staged AM header bytes included in "
            "copy/ownership (default: 4K)\n"
            "  --summary-only            print only SUMMARY lines on role a\n"
            "  --help                    show this message\n"
            "\n"
            "If both memids are omitted, UCX_OBMM_CC_MEMIDS is parsed as\n"
            "<local_export_cc_memid>,<peer_import_cc_memid>.\n",
            prog, prog);
}

static int parse_role(const char *str, role_t *role_p)
{
    if (strcmp(str, "a") == 0) {
        *role_p = ROLE_A;
    } else if (strcmp(str, "b") == 0) {
        *role_p = ROLE_B;
    } else {
        return -1;
    }

    return 0;
}

static int parse_size_list(const char *str, options_t *opts)
{
    char     *copy;
    char     *saveptr = NULL;
    char     *token;
    unsigned count = 0;

    copy = strdup(str);
    if (copy == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    for (token = strtok_r(copy, ",", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ",", &saveptr)) {
        if (count >= MAX_SIZES) {
            free(copy);
            return -1;
        }
        if (parse_size(token, &opts->sizes[count]) != 0) {
            free(copy);
            return -1;
        }
        ++count;
    }

    free(copy);
    if (count == 0) {
        return -1;
    }

    opts->size_count = count;
    return 0;
}

static int parse_batch_list(const char *str, options_t *opts)
{
    char     *copy;
    char     *saveptr = NULL;
    char     *token;
    unsigned count = 0;

    copy = strdup(str);
    if (copy == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    for (token = strtok_r(copy, ",", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ",", &saveptr)) {
        unsigned long value;
        char          *end;

        if (count >= MAX_BATCHES) {
            free(copy);
            return -1;
        }

        errno = 0;
        value = strtoul(token, &end, 0);
        if ((errno != 0) || (end == token) || (*end != '\0') ||
            (value == 0) || (value > UINT_MAX)) {
            free(copy);
            return -1;
        }

        opts->batches[count++] = (unsigned)value;
    }

    free(copy);
    if (count == 0) {
        return -1;
    }

    opts->batch_count = count;
    return 0;
}

static int parse_env_cc_memids(char *local_path, size_t local_len,
                               char *peer_path, size_t peer_len)
{
    const char *env = getenv("UCX_OBMM_CC_MEMIDS");
    char       *copy;
    char       *comma;

    if ((env == NULL) || (*env == '\0')) {
        return -1;
    }

    copy = strdup(env);
    if (copy == NULL) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    comma = strchr(copy, ',');
    if (comma == NULL) {
        free(copy);
        return -1;
    }
    *comma = '\0';

    set_dev_from_memid(local_path, local_len, copy);
    set_dev_from_memid(peer_path, peer_len, comma + 1);
    free(copy);
    return 0;
}

static void parse_args(int argc, char **argv, options_t *opts)
{
    int i;

    options_init(opts);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--role") == 0) {
            if ((++i >= argc) || (parse_role(argv[i], &opts->role) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--connect") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            snprintf(opts->connect_ep, sizeof(opts->connect_ep), "%s",
                     argv[i]);
        } else if (strcmp(argv[i], "--listen") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            snprintf(opts->listen_ep, sizeof(opts->listen_ep), "%s",
                     argv[i]);
        } else if (strcmp(argv[i], "--local-export-memid") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            set_dev_from_memid(opts->local_dev, sizeof(opts->local_dev),
                               argv[i]);
        } else if (strcmp(argv[i], "--peer-import-memid") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            set_dev_from_memid(opts->peer_dev, sizeof(opts->peer_dev),
                               argv[i]);
        } else if (strcmp(argv[i], "--local-dev") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            snprintf(opts->local_dev, sizeof(opts->local_dev), "%s", argv[i]);
        } else if (strcmp(argv[i], "--peer-dev") == 0) {
            if (++i >= argc) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            snprintf(opts->peer_dev, sizeof(opts->peer_dev), "%s", argv[i]);
        } else if (strcmp(argv[i], "--sizes") == 0) {
            if ((++i >= argc) || (parse_size_list(argv[i], opts) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--batches") == 0) {
            if ((++i >= argc) || (parse_batch_list(argv[i], opts) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
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
        } else if (strcmp(argv[i], "--offset") == 0) {
            if ((++i >= argc) || (parse_size(argv[i], &opts->offset) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--own-granule") == 0) {
            if ((++i >= argc) ||
                (parse_size(argv[i], &opts->own_granule) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--header-size") == 0) {
            if ((++i >= argc) ||
                (parse_size(argv[i], &opts->header_size) != 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(argv[i], "--summary-only") == 0) {
            opts->summary_only = 1;
        } else {
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if ((opts->local_dev[0] == '\0') && (opts->peer_dev[0] == '\0')) {
        (void)parse_env_cc_memids(opts->local_dev, sizeof(opts->local_dev),
                                  opts->peer_dev, sizeof(opts->peer_dev));
    }

    if ((opts->role == ROLE_UNSET) ||
        ((opts->role == ROLE_A) && (opts->connect_ep[0] == '\0')) ||
        ((opts->role == ROLE_B) && (opts->listen_ep[0] == '\0')) ||
        ((opts->role == ROLE_A) && (opts->peer_dev[0] == '\0')) ||
        ((opts->role == ROLE_B) && (opts->local_dev[0] == '\0')) ||
        (opts->iters == 0)) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }
}

static obmm_set_ownership_fn_t load_set_ownership(void)
{
    obmm_set_ownership_fn_t fn;
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

    fn = (obmm_set_ownership_fn_t)sym;
    return fn;
}

static int set_owner(obmm_set_ownership_fn_t fn, const mapped_region_t *region,
                     size_t offset, size_t length, int prot)
{
    void *start = (char*)region->addr + offset;
    void *end   = (char*)start + length;

    return fn(region->fd, start, end, prot);
}

static mapped_region_t map_region(const char *path, size_t map_size)
{
    mapped_region_t region;

    memset(&region, 0, sizeof(region));
    snprintf(region.path, sizeof(region.path), "%s", path);
    region.size = map_size;

    region.fd = open(path, O_RDWR);
    if (region.fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    region.addr = mmap(NULL, map_size, PROT_NONE, MAP_SHARED, region.fd, 0);
    if (region.addr == MAP_FAILED) {
        fprintf(stderr, "mmap(%s, %zu) failed: %s\n", path, map_size,
                strerror(errno));
        close(region.fd);
        exit(EXIT_FAILURE);
    }

    return region;
}

static void unmap_region(obmm_set_ownership_fn_t fn, mapped_region_t *region)
{
    if (region->addr != NULL) {
        (void)set_owner(fn, region, 0, region->size, PROT_NONE);
        munmap(region->addr, region->size);
        region->addr = NULL;
    }
    if (region->fd >= 0) {
        close(region->fd);
        region->fd = -1;
    }
}

static void *xaligned_alloc(size_t alignment, size_t size)
{
    void *ptr;

    if (size == 0) {
        size = 1;
    }
    if (posix_memalign(&ptr, alignment, size) != 0) {
        perror("posix_memalign");
        exit(EXIT_FAILURE);
    }

    return ptr;
}

static void fill_source(uint8_t *ptr, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        ptr[i] = (uint8_t)((i * 33u) + 17u);
    }
}

static uint64_t checksum_bytes(const uint8_t *ptr, size_t length)
{
    uint64_t sum = 0;
    size_t   i;

    if (length == 0) {
        return 0;
    }

    for (i = 0; i < length; i += 4096u) {
        sum += ptr[i];
    }
    sum += ptr[length - 1u];
    return sum;
}

static int run_batch_writer(obmm_set_ownership_fn_t fn,
                            const mapped_region_t *region, size_t offset,
                            size_t own_len, size_t msg_stride,
                            size_t total_len, unsigned batch_count,
                            const void *src, sample_t *sample)
{
    uint64_t t0, t1, t2, t3;
    char     *dst_base;
    unsigned i;

    memset(sample, 0, sizeof(*sample));
    dst_base = (char*)region->addr + offset;

    t0 = now_nsec();
    if (set_owner(fn, region, offset, own_len, PROT_WRITE) != 0) {
        return -1;
    }
    t1 = now_nsec();

    for (i = 0; i < batch_count; ++i) {
        memcpy(dst_base + ((size_t)i * msg_stride), src, total_len);
    }
    t2 = now_nsec();

    if (set_owner(fn, region, offset, own_len, PROT_NONE) != 0) {
        return -1;
    }
    t3 = now_nsec();

    sample->acq      = t1 - t0;
    sample->copy     = t2 - t1;
    sample->release  = t3 - t2;
    sample->total    = t3 - t0;
    sample->checksum = 0;
    return 0;
}

static int run_batch_reader(obmm_set_ownership_fn_t fn,
                            const mapped_region_t *region, size_t offset,
                            size_t own_len, size_t msg_stride,
                            size_t total_len, unsigned batch_count,
                            void *dst, sample_t *sample)
{
    uint64_t t0, t1, t2, t3;
    char     *src_base;
    char     *dst_base = dst;
    unsigned i;

    memset(sample, 0, sizeof(*sample));
    src_base = (char*)region->addr + offset;

    t0 = now_nsec();
    if (set_owner(fn, region, offset, own_len, PROT_READ) != 0) {
        return -1;
    }
    t1 = now_nsec();

    for (i = 0; i < batch_count; ++i) {
        char *dst_msg = dst_base + ((size_t)i * total_len);

        memcpy(dst_msg, src_base + ((size_t)i * msg_stride), total_len);
        sample->checksum += checksum_bytes((const uint8_t*)dst_msg,
                                           total_len);
    }
    t2 = now_nsec();

    if (set_owner(fn, region, offset, own_len, PROT_NONE) != 0) {
        return -1;
    }
    t3 = now_nsec();

    sample->acq      = t1 - t0;
    sample->copy     = t2 - t1;
    sample->release  = t3 - t2;
    sample->total    = t3 - t0;
    g_checksum      += sample->checksum;
    return 0;
}

static int split_endpoint(const char *endpoint, int allow_port_only,
                          char *host, size_t host_len, char *port,
                          size_t port_len)
{
    const char *colon = strrchr(endpoint, ':');
    size_t      host_part_len;

    if (colon == NULL) {
        if (!allow_port_only) {
            return -1;
        }
        host[0] = '\0';
        snprintf(port, port_len, "%s", endpoint);
        return 0;
    }

    if (*(colon + 1) == '\0') {
        return -1;
    }

    host_part_len = (size_t)(colon - endpoint);
    if (host_part_len >= host_len) {
        return -1;
    }

    memcpy(host, endpoint, host_part_len);
    host[host_part_len] = '\0';
    snprintf(port, port_len, "%s", colon + 1);
    return 0;
}

static int connect_tcp(const char *endpoint)
{
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *ai;
    char            host[256];
    char            port[32];
    int             sock = -1;
    int             rc;

    if (split_endpoint(endpoint, 0, host, sizeof(host), port, sizeof(port)) !=
        0) {
        fprintf(stderr, "invalid --connect endpoint: %s\n", endpoint);
        exit(EXIT_FAILURE);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", host, port,
                gai_strerror(rc));
        exit(EXIT_FAILURE);
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    if (sock < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n", endpoint, strerror(errno));
        exit(EXIT_FAILURE);
    }

    return sock;
}

static int listen_tcp(const char *endpoint)
{
    struct addrinfo hints;
    struct addrinfo *res;
    struct addrinfo *ai;
    char            host[256];
    char            port[32];
    int             listen_fd = -1;
    int             sock;
    int             opt = 1;
    int             rc;

    if (split_endpoint(endpoint, 1, host, sizeof(host), port, sizeof(port)) !=
        0) {
        fprintf(stderr, "invalid --listen endpoint: %s\n", endpoint);
        exit(EXIT_FAILURE);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_flags    = AI_PASSIVE;

    rc = getaddrinfo((host[0] == '\0') ? NULL : host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n",
                (host[0] == '\0') ? "*" : host, port, gai_strerror(rc));
        exit(EXIT_FAILURE);
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        listen_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }

        (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                         sizeof(opt));
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
        exit(EXIT_FAILURE);
    }

    sock = accept(listen_fd, NULL, NULL);
    if (sock < 0) {
        fprintf(stderr, "accept(%s) failed: %s\n", endpoint, strerror(errno));
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    close(listen_fd);
    return sock;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *ptr = buf;

    while (len > 0) {
        ssize_t n = send(fd, ptr, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        ptr += n;
        len -= (size_t)n;
    }

    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *ptr = buf;

    while (len > 0) {
        ssize_t n = recv(fd, ptr, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        ptr += n;
        len -= (size_t)n;
    }

    return 0;
}

static uint64_t result_field(const iter_result_t *result, unsigned field)
{
    switch (field) {
    case 0:
        return result->writer.acq;
    case 1:
        return result->writer.copy;
    case 2:
        return result->writer.release;
    case 3:
        return result->writer.total;
    case 4:
        return result->reader.acq;
    case 5:
        return result->reader.copy;
    case 6:
        return result->reader.release;
    case 7:
        return result->reader.total;
    case 8:
        return result->path_total;
    default:
        return result->wall;
    }
}

static double median_result_field(const iter_result_t *results, unsigned count,
                                  unsigned field)
{
    uint64_t *values;
    uint64_t  median;
    unsigned  i;

    values = malloc(sizeof(*values) * count);
    if (values == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < count; ++i) {
        values[i] = result_field(&results[i], field);
    }

    median = median_u64(values, count);
    free(values);
    return usec(median);
}

static void summarize_results(const iter_result_t *results, unsigned count,
                              median_result_t *summary)
{
    summary->writer_acq     = median_result_field(results, count, 0);
    summary->writer_copy    = median_result_field(results, count, 1);
    summary->writer_release = median_result_field(results, count, 2);
    summary->writer_total   = median_result_field(results, count, 3);
    summary->reader_acq     = median_result_field(results, count, 4);
    summary->reader_copy    = median_result_field(results, count, 5);
    summary->reader_release = median_result_field(results, count, 6);
    summary->reader_total   = median_result_field(results, count, 7);
    summary->path_total     = median_result_field(results, count, 8);
    summary->wall           = median_result_field(results, count, 9);
}

static int run_batch_case_a(int sock, obmm_set_ownership_fn_t fn,
                            const mapped_region_t *peer, size_t offset,
                            size_t own_len, size_t msg_stride,
                            size_t total_len, unsigned batch_count,
                            const void *src, unsigned warmup, unsigned iters,
                            uint64_t *seq_p, median_result_t *summary)
{
    iter_result_t *results;
    unsigned      total_iters = warmup + iters;
    unsigned      out = 0;
    unsigned      i;
    ctrl_cmd_t    cmd;
    ctrl_reply_t  reply;
    sample_t      writer;
    uint64_t      wall_start;
    uint64_t      wall_end;
    int           ret = -1;

    results = calloc(iters, sizeof(*results));
    if (results == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < total_iters; ++i) {
        wall_start = now_nsec();
        if (run_batch_writer(fn, peer, offset, own_len, msg_stride, total_len,
                             batch_count, src, &writer) != 0) {
            fprintf(stderr, "writer failed errno=%d (%s)\n", errno,
                    strerror(errno));
            goto out;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.magic       = CTRL_MAGIC;
        cmd.version     = CTRL_VERSION;
        cmd.type        = CMD_READ_BATCH;
        cmd.seq         = (*seq_p)++;
        cmd.offset      = offset;
        cmd.own_len     = own_len;
        cmd.msg_stride  = msg_stride;
        cmd.total_len   = total_len;
        cmd.batch_count = batch_count;

        if (send_all(sock, &cmd, sizeof(cmd)) != 0) {
            fprintf(stderr, "send command failed: %s\n", strerror(errno));
            goto out;
        }
        if (recv_all(sock, &reply, sizeof(reply)) != 0) {
            fprintf(stderr, "receive reply failed: %s\n", strerror(errno));
            goto out;
        }
        wall_end = now_nsec();

        if ((reply.magic != CTRL_MAGIC) || (reply.version != CTRL_VERSION) ||
            (reply.seq != cmd.seq) || (reply.status != 0)) {
            fprintf(stderr,
                    "invalid reply seq=%" PRIu64
                    " status=%u magic=%" PRIx64 " version=%u\n",
                    cmd.seq, reply.status, reply.magic, reply.version);
            goto out;
        }

        if (i >= warmup) {
            results[out].writer     = writer;
            results[out].reader     = reply.sample;
            results[out].path_total = writer.total + reply.sample.total;
            results[out].wall       = wall_end - wall_start;
            ++out;
        }
    }

    summarize_results(results, out, summary);
    ret = 0;

out:
    free(results);
    return ret;
}

static void print_case_row(size_t payload_len, size_t total_len,
                           unsigned batch_count, size_t msg_stride,
                           size_t own_len, const median_result_t *summary)
{
    printf("batch,%zu,%zu,%u,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,"
           "%.3f,%.3f,%.3f,%.3f,%.3f\n",
           payload_len, total_len, batch_count, msg_stride, own_len,
           summary->writer_acq, summary->writer_copy,
           summary->writer_release, summary->writer_total,
           summary->reader_acq, summary->reader_copy,
           summary->reader_release, summary->reader_total,
           summary->path_total, summary->wall);
}

static void print_summary_line(size_t payload_len, size_t total_len,
                               unsigned batch_count, unsigned base_batch,
                               size_t msg_stride, size_t own_len,
                               const median_result_t *summary,
                               double base_per_msg)
{
    double batch = (double)batch_count;
    double per_msg = summary->path_total / batch;
    double wall_per_msg = summary->wall / batch;
    double speedup = -1.0;

    if (base_per_msg > 0.0) {
        speedup = base_per_msg / per_msg;
    }

    printf("SUMMARY size=%zu batch=%u base_batch=%u total_len=%zu "
           "msg_stride=%zu own_len=%zu per_msg_us=%.3f speedup=%.3f "
           "wall_per_msg_us=%.3f writer_own_per_msg_us=%.3f "
           "writer_copy_per_msg_us=%.3f reader_own_per_msg_us=%.3f "
           "reader_copy_per_msg_us=%.3f\n",
           payload_len, batch_count, base_batch, total_len, msg_stride,
           own_len, per_msg, speedup, wall_per_msg,
           (summary->writer_acq + summary->writer_release) / batch,
           summary->writer_copy / batch,
           (summary->reader_acq + summary->reader_release) / batch,
           summary->reader_copy / batch);
}

static int run_role_a(const options_t *opts, obmm_set_ownership_fn_t fn,
                      const mapped_region_t *peer, size_t page_size)
{
    int       sock;
    uint8_t  *src;
    size_t    max_total = 0;
    uint64_t  seq = 1;
    ctrl_cmd_t stop;
    unsigned  i;
    int       ret = EXIT_SUCCESS;

    for (i = 0; i < opts->size_count; ++i) {
        size_t total_len = opts->sizes[i] + opts->header_size;
        if (total_len > max_total) {
            max_total = total_len;
        }
    }

    src = xaligned_alloc(page_size, max_total);
    fill_source(src, max_total);
    sock = connect_tcp(opts->connect_ep);

    if (!opts->summary_only) {
        printf("# obmm_cc_batch_path_probe role=a peer=%s map_size=%zu "
               "offset=%zu own_granule=%zu header_size=%zu iters=%u "
               "warmup=%u\n",
               opts->peer_dev, opts->map_size, opts->offset,
               opts->own_granule, opts->header_size, opts->iters,
               opts->warmup);
        printf("section,payload_len,total_len,batch_count,msg_stride,own_len,"
               "writer_acq_us,writer_copy_us,writer_release_us,"
               "writer_total_us,reader_acq_us,reader_copy_us,"
               "reader_release_us,reader_total_us,path_total_us,wall_us\n");
    }

    for (i = 0; i < opts->size_count; ++i) {
        size_t   payload_len = opts->sizes[i];
        size_t   total_len   = payload_len + opts->header_size;
        size_t   msg_stride  = align_up(total_len, opts->own_granule);
        double   base_per_msg = -1.0;
        unsigned base_batch = 0;
        unsigned j;

        for (j = 0; j < opts->batch_count; ++j) {
            median_result_t summary;
            unsigned        batch_count = opts->batches[j];
            size_t          own_len = msg_stride * (size_t)batch_count;

            if (run_batch_case_a(sock, fn, peer, opts->offset, own_len,
                                 msg_stride, total_len, batch_count, src,
                                 opts->warmup, opts->iters, &seq,
                                 &summary) != 0) {
                ret = EXIT_FAILURE;
                goto out;
            }

            if (!opts->summary_only) {
                print_case_row(payload_len, total_len, batch_count,
                               msg_stride, own_len, &summary);
            }

            if (base_per_msg < 0.0) {
                base_per_msg = summary.path_total / (double)batch_count;
                base_batch   = batch_count;
            }

            print_summary_line(payload_len, total_len, batch_count,
                               base_batch, msg_stride, own_len, &summary,
                               base_per_msg);
        }
    }

out:
    memset(&stop, 0, sizeof(stop));
    stop.magic   = CTRL_MAGIC;
    stop.version = CTRL_VERSION;
    stop.type    = CMD_STOP;
    stop.seq     = seq++;
    (void)send_all(sock, &stop, sizeof(stop));

    close(sock);
    free(src);
    return ret;
}

static int run_role_b(const options_t *opts, obmm_set_ownership_fn_t fn,
                      const mapped_region_t *local, size_t page_size)
{
    int          sock;
    uint8_t     *dst;
    size_t       max_total = 0;
    unsigned     max_batch = 1;
    unsigned     i;
    ctrl_cmd_t   cmd;
    ctrl_reply_t reply;
    int          ret = EXIT_SUCCESS;

    for (i = 0; i < opts->size_count; ++i) {
        size_t total_len = opts->sizes[i] + opts->header_size;
        if (total_len > max_total) {
            max_total = total_len;
        }
    }
    for (i = 0; i < opts->batch_count; ++i) {
        if (opts->batches[i] > max_batch) {
            max_batch = opts->batches[i];
        }
    }

    dst  = xaligned_alloc(page_size, max_total * (size_t)max_batch);
    sock = listen_tcp(opts->listen_ep);

    while (1) {
        if (recv_all(sock, &cmd, sizeof(cmd)) != 0) {
            fprintf(stderr, "receive command failed: %s\n", strerror(errno));
            ret = EXIT_FAILURE;
            break;
        }

        if ((cmd.magic != CTRL_MAGIC) || (cmd.version != CTRL_VERSION)) {
            fprintf(stderr, "invalid command magic/version\n");
            ret = EXIT_FAILURE;
            break;
        }

        if (cmd.type == CMD_STOP) {
            break;
        }
        if (cmd.type != CMD_READ_BATCH) {
            fprintf(stderr, "invalid command type: %u\n", cmd.type);
            ret = EXIT_FAILURE;
            break;
        }

        memset(&reply, 0, sizeof(reply));
        reply.magic   = CTRL_MAGIC;
        reply.version = CTRL_VERSION;
        reply.seq     = cmd.seq;

        if ((cmd.batch_count == 0) || (cmd.batch_count > max_batch) ||
            (cmd.offset > local->size) ||
            (cmd.own_len > (local->size - cmd.offset)) ||
            ((cmd.offset % page_size) != 0) ||
            ((cmd.own_len % page_size) != 0) ||
            ((cmd.msg_stride % page_size) != 0) ||
            (cmd.total_len > cmd.msg_stride) ||
            (cmd.total_len > max_total) ||
            (cmd.msg_stride > cmd.own_len) ||
            (((uint64_t)cmd.batch_count * cmd.msg_stride) > cmd.own_len)) {
            reply.status = EINVAL;
        } else if (run_batch_reader(fn, local, (size_t)cmd.offset,
                                    (size_t)cmd.own_len,
                                    (size_t)cmd.msg_stride,
                                    (size_t)cmd.total_len,
                                    cmd.batch_count, dst,
                                    &reply.sample) != 0) {
            reply.status = (uint32_t)errno;
        }

        if (send_all(sock, &reply, sizeof(reply)) != 0) {
            fprintf(stderr, "send reply failed: %s\n", strerror(errno));
            ret = EXIT_FAILURE;
            break;
        }

        if (reply.status != 0) {
            fprintf(stderr, "reader failed status=%u\n", reply.status);
            ret = EXIT_FAILURE;
            break;
        }
    }

    close(sock);
    free(dst);
    return ret;
}

static void validate_options(options_t *opts, size_t page_size)
{
    size_t   max_own = 0;
    size_t   max_total = 0;
    unsigned max_batch = 1;
    unsigned i, j;

    opts->map_size = align_up(opts->map_size, page_size);

    if (!is_power_of_two(opts->own_granule) ||
        ((opts->own_granule % page_size) != 0)) {
        fprintf(stderr,
                "--own-granule must be a power-of-two page multiple\n");
        exit(EXIT_FAILURE);
    }
    if ((opts->offset % page_size) != 0) {
        fprintf(stderr, "--offset must be page-aligned\n");
        exit(EXIT_FAILURE);
    }
    if (opts->warmup > (UINT_MAX - opts->iters)) {
        fprintf(stderr, "invalid iteration counts\n");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < opts->size_count; ++i) {
        size_t total_len = opts->sizes[i] + opts->header_size;
        size_t msg_stride;

        if (total_len < opts->sizes[i]) {
            fprintf(stderr, "invalid size/header combination\n");
            exit(EXIT_FAILURE);
        }
        if (total_len > (SIZE_MAX - (opts->own_granule - 1u))) {
            fprintf(stderr, "message stride overflow\n");
            exit(EXIT_FAILURE);
        }
        if (total_len > max_total) {
            max_total = total_len;
        }

        msg_stride = align_up(total_len, opts->own_granule);
        for (j = 0; j < opts->batch_count; ++j) {
            size_t own_len;

            if ((opts->batches[j] == 0) ||
                ((size_t)opts->batches[j] > (SIZE_MAX / msg_stride))) {
                fprintf(stderr, "batch range overflow\n");
                exit(EXIT_FAILURE);
            }
            if (opts->batches[j] > max_batch) {
                max_batch = opts->batches[j];
            }
            own_len = msg_stride * (size_t)opts->batches[j];
            if (own_len > max_own) {
                max_own = own_len;
            }
        }
    }

    if ((max_batch != 0) && (max_total > (SIZE_MAX / max_batch))) {
        fprintf(stderr, "receive buffer size overflow\n");
        exit(EXIT_FAILURE);
    }

    if ((opts->offset > opts->map_size) ||
        (max_own > (opts->map_size - opts->offset))) {
        fprintf(stderr,
                "test range does not fit map: offset=%zu max_own=%zu "
                "map_size=%zu\n",
                opts->offset, max_own, opts->map_size);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    options_t               opts;
    obmm_set_ownership_fn_t set_ownership;
    mapped_region_t         region;
    long                    page_size_l;
    size_t                  page_size;
    int                     ret;

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    parse_args(argc, argv, &opts);

    page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0) {
        fprintf(stderr, "failed to get page size\n");
        return EXIT_FAILURE;
    }
    page_size = (size_t)page_size_l;
    validate_options(&opts, page_size);

    set_ownership = load_set_ownership();

    if (opts.role == ROLE_A) {
        region = map_region(opts.peer_dev, opts.map_size);
    } else {
        region = map_region(opts.local_dev, opts.map_size);
    }

    if (set_owner(set_ownership, &region, 0, opts.map_size, PROT_NONE) != 0) {
        fprintf(stderr, "initial PROT_NONE ownership setup failed: %s\n",
                strerror(errno));
        unmap_region(set_ownership, &region);
        return EXIT_FAILURE;
    }

    if (opts.role == ROLE_A) {
        ret = run_role_a(&opts, set_ownership, &region, page_size);
    } else {
        ret = run_role_b(&opts, set_ownership, &region, page_size);
    }

    unmap_region(set_ownership, &region);

    if (g_checksum == 0xdeadbeefull) {
        fprintf(stderr, "checksum guard=%" PRIu64 "\n", g_checksum);
    }

    return ret;
}
