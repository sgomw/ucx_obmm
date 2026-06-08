#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define OBMM_SYSFS_ROOT "/sys/devices/obmm"
#define OBMM_DEV_FMT "/dev/obmm_shmdev%" PRIu64

typedef enum {
    MAP_MODE_AUTO = 0,
    MAP_MODE_CC,
    MAP_MODE_NC
} map_mode_t;

typedef struct {
    uint64_t   memids[256];
    unsigned   num_memids;
    uint64_t   size;
    map_mode_t mode;
    int        have_memids;
    int        have_size;
} options_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --memids <id[,id...]> --size <bytes|K|M|G|T> "
            "[--map auto|cc|nc]\n"
            "\n"
            "Examples:\n"
            "  %s --memids 1 --size 3G\n"
            "  %s --memids 7,8 --size 512M --map cc\n"
            "  %s --memids 9,10 --size 3G --map nc\n",
            prog, prog, prog, prog);
}

static int parse_u64(const char *text, uint64_t *value)
{
    char               *end;
    unsigned long long  parsed;

    errno  = 0;
    parsed = strtoull(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0')) {
        return -1;
    }

    *value = (uint64_t)parsed;
    return 0;
}

static int parse_size(const char *text, uint64_t *value)
{
    static const struct {
        const char *suffix;
        uint64_t    scale;
    } suffixes[] = {
        {"KIB", 1024ull},
        {"MIB", 1024ull * 1024ull},
        {"GIB", 1024ull * 1024ull * 1024ull},
        {"TIB", 1024ull * 1024ull * 1024ull * 1024ull},
        {"KB",  1024ull},
        {"MB",  1024ull * 1024ull},
        {"GB",  1024ull * 1024ull * 1024ull},
        {"TB",  1024ull * 1024ull * 1024ull * 1024ull},
        {"K",   1024ull},
        {"M",   1024ull * 1024ull},
        {"G",   1024ull * 1024ull * 1024ull},
        {"T",   1024ull * 1024ull * 1024ull * 1024ull}
    };
    char               *end;
    unsigned long long  base;
    char                suffix_buf[16];
    size_t              i;
    size_t              suffix_len;

    errno = 0;
    base  = strtoull(text, &end, 0);
    if ((errno != 0) || (end == text)) {
        return -1;
    }

    if (*end == '\0') {
        *value = (uint64_t)base;
        return 0;
    }

    suffix_len = strlen(end);
    if (suffix_len >= sizeof(suffix_buf)) {
        return -1;
    }

    for (i = 0; i < suffix_len; ++i) {
        suffix_buf[i] = (char)toupper((unsigned char)end[i]);
    }
    suffix_buf[suffix_len] = '\0';

    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        if (strcmp(suffix_buf, suffixes[i].suffix) == 0) {
            if (base > (ULLONG_MAX / suffixes[i].scale)) {
                errno = ERANGE;
                return -1;
            }
            *value = (uint64_t)base * suffixes[i].scale;
            return 0;
        }
    }

    return -1;
}

static int parse_mode(const char *text, map_mode_t *mode)
{
    if (strcmp(text, "auto") == 0) {
        *mode = MAP_MODE_AUTO;
        return 0;
    }
    if (strcmp(text, "cc") == 0) {
        *mode = MAP_MODE_CC;
        return 0;
    }
    if (strcmp(text, "nc") == 0) {
        *mode = MAP_MODE_NC;
        return 0;
    }
    return -1;
}

static int parse_memid_list(const char *text, options_t *opts)
{
    char     *copy;
    char     *cursor;
    char     *next;
    uint64_t  memid;
    unsigned  i;

    copy = strdup(text);
    if (copy == NULL) {
        fprintf(stderr, "failed to allocate memid list buffer\n");
        return -1;
    }

    cursor = copy;
    while (cursor != NULL) {
        next = strchr(cursor, ',');
        if (next != NULL) {
            *next = '\0';
            ++next;
        }

        if ((*cursor == '\0') || (parse_u64(cursor, &memid) != 0) ||
            (memid == 0)) {
            free(copy);
            return -1;
        }

        for (i = 0; i < opts->num_memids; ++i) {
            if (opts->memids[i] == memid) {
                free(copy);
                fprintf(stderr, "duplicate memid in --memids: %" PRIu64 "\n",
                        memid);
                return -1;
            }
        }

        if (opts->num_memids >= (sizeof(opts->memids) / sizeof(opts->memids[0]))) {
            free(copy);
            fprintf(stderr, "too many memids, max=%zu\n",
                    sizeof(opts->memids) / sizeof(opts->memids[0]));
            return -1;
        }

        opts->memids[opts->num_memids++] = memid;
        cursor = next;
    }

    free(copy);
    return (opts->num_memids > 0) ? 0 : -1;
}

static int parse_args(int argc, char **argv, options_t *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->mode = MAP_MODE_AUTO;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--memids") == 0) {
            if ((i + 1 >= argc) ||
                (parse_memid_list(argv[++i], opts) != 0)) {
                fprintf(stderr, "invalid --memids value\n");
                return -1;
            }
            opts->have_memids = 1;
        } else if (strcmp(argv[i], "--size") == 0) {
            if ((i + 1 >= argc) || (parse_size(argv[++i], &opts->size) != 0) ||
                (opts->size == 0)) {
                fprintf(stderr, "invalid --size value\n");
                return -1;
            }
            opts->have_size = 1;
        } else if (strcmp(argv[i], "--map") == 0) {
            if ((i + 1 >= argc) || (parse_mode(argv[++i], &opts->mode) != 0)) {
                fprintf(stderr, "invalid --map value, expected auto|cc|nc\n");
                return -1;
            }
        } else if ((strcmp(argv[i], "-h") == 0) ||
                   (strcmp(argv[i], "--help") == 0)) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (!opts->have_memids || !opts->have_size) {
        fprintf(stderr, "--memids and --size are required\n");
        return -1;
    }

    return 0;
}

static int read_text_file(const char *path, char *buf, size_t buf_len)
{
    ssize_t nread;
    int     fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    nread = read(fd, buf, buf_len - 1);
    close(fd);
    if (nread < 0) {
        return -1;
    }

    while ((nread > 0) &&
           ((buf[nread - 1] == '\n') || (buf[nread - 1] == '\r') ||
            (buf[nread - 1] == ' ') || (buf[nread - 1] == '\t'))) {
        --nread;
    }

    buf[nread] = '\0';
    return 0;
}

static int sysfs_read_type(uint64_t memid, char *type_buf, size_t type_buf_len)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/obmm_shmdev%" PRIu64 "/type",
             OBMM_SYSFS_ROOT, memid);
    return read_text_file(path, type_buf, type_buf_len);
}

static int sysfs_read_allow_mmap(uint64_t memid, int *allow_mmap)
{
    char path[PATH_MAX];
    char buf[32];

    snprintf(path, sizeof(path), "%s/obmm_shmdev%" PRIu64 "/allow_mmap",
             OBMM_SYSFS_ROOT, memid);
    if (read_text_file(path, buf, sizeof(buf)) != 0) {
        return -1;
    }

    *allow_mmap = (strcmp(buf, "0") != 0);
    return 0;
}

static int sysfs_read_size(uint64_t memid, uint64_t *size)
{
    char path[PATH_MAX];
    char buf[64];

    snprintf(path, sizeof(path), "%s/obmm_shmdev%" PRIu64 "/size",
             OBMM_SYSFS_ROOT, memid);
    if (read_text_file(path, buf, sizeof(buf)) != 0) {
        return -1;
    }

    return parse_u64(buf, size);
}

static int clear_once(uint64_t memid, uint64_t size, int use_sync,
                      const char *label)
{
    char   dev_path[PATH_MAX];
    void  *map;
    int    fd;
    int    open_flags;

    snprintf(dev_path, sizeof(dev_path), OBMM_DEV_FMT, memid);

    open_flags = O_RDWR | O_CLOEXEC;
    if (use_sync) {
        open_flags |= O_SYNC;
    }

    fd = open(dev_path, open_flags);
    if (fd < 0) {
        fprintf(stderr, "%s open(%s) failed: %s\n",
                label, dev_path, strerror(errno));
        return -1;
    }

    map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "%s mmap(%s, 0x%" PRIx64 ") failed: %s\n",
                label, dev_path, size, strerror(errno));
        close(fd);
        return -1;
    }

    memset(map, 0, (size_t)size);
    __sync_synchronize();

    if (msync(map, (size_t)size, MS_SYNC) != 0) {
        fprintf(stderr, "%s msync warning: %s\n", label, strerror(errno));
    }

    if (munmap(map, (size_t)size) != 0) {
        fprintf(stderr, "%s munmap failed: %s\n", label, strerror(errno));
        close(fd);
        return -1;
    }

    if (close(fd) != 0) {
        fprintf(stderr, "%s close failed: %s\n", label, strerror(errno));
        return -1;
    }

    printf("cleared memid=%" PRIu64 " size=%" PRIu64 " bytes via %s mapping\n",
           memid, size, label);
    return 0;
}

int main(int argc, char **argv)
{
    options_t opts;
    char      type_buf[32];
    uint64_t  sysfs_size;
    int       allow_mmap;
    unsigned  i;
    int       status = 0;

    if (parse_args(argc, argv, &opts) != 0) {
        usage(argv[0]);
        return 2;
    }

    for (i = 0; i < opts.num_memids; ++i) {
        uint64_t memid = opts.memids[i];

        if (sysfs_read_type(memid, type_buf, sizeof(type_buf)) != 0) {
            fprintf(stderr,
                    "failed to read sysfs type for memid=%" PRIu64 ": %s\n",
                    memid, strerror(errno));
            status = 1;
            continue;
        }

        if (strcmp(type_buf, "export") != 0) {
            fprintf(stderr, "memid=%" PRIu64 " is type=%s, not local export\n",
                    memid, type_buf);
            status = 1;
            continue;
        }

        if ((sysfs_read_allow_mmap(memid, &allow_mmap) != 0) || !allow_mmap) {
            fprintf(stderr, "memid=%" PRIu64 " does not support mmap\n", memid);
            status = 1;
            continue;
        }

        if (sysfs_read_size(memid, &sysfs_size) != 0) {
            fprintf(stderr,
                    "failed to read sysfs size for memid=%" PRIu64 ": %s\n",
                    memid, strerror(errno));
            status = 1;
            continue;
        }

        if (opts.size > sysfs_size) {
            fprintf(stderr,
                    "requested size=%" PRIu64 " exceeds sysfs size=%" PRIu64
                    " for memid=%" PRIu64 "\n",
                    opts.size, sysfs_size, memid);
            status = 1;
            continue;
        }

        switch (opts.mode) {
        case MAP_MODE_CC:
            if (clear_once(memid, opts.size, 0, "cc") != 0) {
                status = 1;
            }
            break;
        case MAP_MODE_NC:
            if (clear_once(memid, opts.size, 1, "nc") != 0) {
                status = 1;
            }
            break;
        case MAP_MODE_AUTO:
            if (clear_once(memid, opts.size, 0, "auto-cc") == 0) {
                break;
            }
            fprintf(stderr, "memid=%" PRIu64 " auto mode retrying with nc mapping\n",
                    memid);
            if (clear_once(memid, opts.size, 1, "auto-nc") != 0) {
                status = 1;
            }
            break;
        default:
            status = 1;
            break;
        }
    }

    return status;
}
