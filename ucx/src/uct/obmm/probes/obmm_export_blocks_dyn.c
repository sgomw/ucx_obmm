/*
 * Standalone OBMM export helper for the block-FIFO UCX layout.
 *
 * This file intentionally does not include libobmm headers and does not link
 * libobmm at build time. It loads libobmm.so at runtime and resolves only
 * obmm_export.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBMM_EXPORT_BLOCKS_DEFAULT_COUNT      96u
#define OBMM_EXPORT_BLOCKS_DEFAULT_SIZE       (34ull * 1024ull * 1024ull)
#define OBMM_EXPORT_BLOCKS_MAX_NUMA_NODES     16u
#define OBMM_EXPORT_BLOCKS_INVALID_MEMID      0ull

typedef uint64_t obmm_mem_id_t;

struct obmm_mem_desc_dyn {
    uint64_t addr;
    uint64_t length;
    uint8_t  seid[16];
    uint8_t  deid[16];
    uint32_t tokenid;
    uint32_t scna;
    uint32_t dcna;
    uint16_t priv_len;
    uint8_t  priv[];
};

typedef obmm_mem_id_t (*obmm_export_func_t)(
        const size_t length[OBMM_EXPORT_BLOCKS_MAX_NUMA_NODES],
        unsigned long flags, struct obmm_mem_desc_dyn *desc);

struct export_record {
    obmm_mem_id_t             memid;
    struct obmm_mem_desc_dyn *desc;
    unsigned                  block_index;
    char                      priv[16];
};

struct options {
    const char   *lib_path;
    unsigned long flags;
    int           flags_set;
    unsigned      first;
    unsigned      count;
    unsigned      numa;
    size_t        block_size;
    uint8_t       deid[16];
    int           deid_set;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --deid HEX32 --flags FLAGS [options]\n"
            "\n"
            "Options:\n"
            "  --lib PATH        libobmm shared object path "
            "(default: libobmm.so)\n"
            "  --count N         number of export blocks (default: 96)\n"
            "  --first N         first block index for priv suffix "
            "(default: 0)\n"
            "  --size BYTES      bytes per export block (default: 34MiB)\n"
            "  --numa N          local NUMA index in the OBMM length array "
            "(default: 0)\n"
            "  --flags FLAGS     numeric obmm_export flags, for example the "
            "target value for FAST|ALLOW_MMAP\n"
            "  --deid HEX32      16-byte destination EID as 32 hex digits\n"
            "  --help            show this help\n"
            "\n"
            "The generated priv bytes are ucx-obmm:00 .. ucx-obmm:95 by "
            "default. The trailing NUL is not included in priv_len.\n",
            prog);
}

static int parse_ulong(const char *arg, unsigned long *value_p)
{
    char *end = NULL;
    errno     = 0;

    *value_p = strtoul(arg, &end, 0);
    return (errno == 0) && (end != arg) && (*end == '\0');
}

static int parse_uint(const char *arg, unsigned *value_p)
{
    unsigned long value;

    if (!parse_ulong(arg, &value) || (value > UINT32_MAX)) {
        return 0;
    }

    *value_p = (unsigned)value;
    return 1;
}

static int parse_size(const char *arg, size_t *value_p)
{
    unsigned long long value;
    char              *end = NULL;
    unsigned long long mul = 1;

    errno = 0;
    value = strtoull(arg, &end, 0);
    if ((errno != 0) || (end == arg)) {
        return 0;
    }

    if (*end != '\0') {
        if ((end[1] != '\0') && !((end[1] == 'i') && (end[2] == 'B') &&
                                  (end[3] == '\0'))) {
            return 0;
        }

        switch (*end) {
        case 'k':
        case 'K':
            mul = 1024ull;
            break;
        case 'm':
        case 'M':
            mul = 1024ull * 1024ull;
            break;
        case 'g':
        case 'G':
            mul = 1024ull * 1024ull * 1024ull;
            break;
        default:
            return 0;
        }
    }

    if (value > (UINT64_MAX / mul)) {
        return 0;
    }

    value *= mul;
    if (value > (unsigned long long)SIZE_MAX) {
        return 0;
    }

    *value_p = (size_t)value;
    return 1;
}

static int hex_value(int c)
{
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex16(const char *arg, uint8_t out[16])
{
    size_t i;

    if (strlen(arg) != 32) {
        return 0;
    }

    for (i = 0; i < 16; ++i) {
        int hi = hex_value(arg[i * 2]);
        int lo = hex_value(arg[i * 2 + 1]);

        if ((hi < 0) || (lo < 0)) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return 1;
}

static int parse_args(int argc, char **argv, struct options *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->lib_path   = "libobmm.so";
    opts->first      = 0;
    opts->count      = OBMM_EXPORT_BLOCKS_DEFAULT_COUNT;
    opts->numa       = 0;
    opts->block_size = (size_t)OBMM_EXPORT_BLOCKS_DEFAULT_SIZE;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "--lib") && (++i < argc)) {
            opts->lib_path = argv[i];
        } else if (!strcmp(argv[i], "--flags") && (++i < argc)) {
            if (!parse_ulong(argv[i], &opts->flags)) {
                fprintf(stderr, "invalid --flags value: %s\n", argv[i]);
                return 0;
            }
            opts->flags_set = 1;
        } else if (!strcmp(argv[i], "--deid") && (++i < argc)) {
            if (!parse_hex16(argv[i], opts->deid)) {
                fprintf(stderr, "invalid --deid value: %s\n", argv[i]);
                return 0;
            }
            opts->deid_set = 1;
        } else if (!strcmp(argv[i], "--first") && (++i < argc)) {
            if (!parse_uint(argv[i], &opts->first)) {
                fprintf(stderr, "invalid --first value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--count") && (++i < argc)) {
            if (!parse_uint(argv[i], &opts->count)) {
                fprintf(stderr, "invalid --count value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--numa") && (++i < argc)) {
            if (!parse_uint(argv[i], &opts->numa)) {
                fprintf(stderr, "invalid --numa value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--size") && (++i < argc)) {
            if (!parse_size(argv[i], &opts->block_size)) {
                fprintf(stderr, "invalid --size value: %s\n", argv[i]);
                return 0;
            }
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            return 0;
        }
    }

    if (!opts->flags_set) {
        fprintf(stderr, "--flags is required because OBMM flags are C macros "
                        "and are not available from libobmm.so\n");
        return 0;
    }
    if (!opts->deid_set) {
        fprintf(stderr, "--deid is required\n");
        return 0;
    }
    if (opts->count == 0) {
        fprintf(stderr, "--count must be greater than zero\n");
        return 0;
    }
    if ((opts->first > 99) || ((opts->first + opts->count - 1) > 99)) {
        fprintf(stderr, "priv format ucx-obmm:NN requires block indexes "
                        "0..99\n");
        return 0;
    }
    if (opts->numa >= OBMM_EXPORT_BLOCKS_MAX_NUMA_NODES) {
        fprintf(stderr, "--numa must be less than %u\n",
                OBMM_EXPORT_BLOCKS_MAX_NUMA_NODES);
        return 0;
    }
    if (opts->block_size == 0) {
        fprintf(stderr, "--size must be greater than zero\n");
        return 0;
    }

    return 1;
}

int main(int argc, char **argv)
{
    struct options       opts;
    struct export_record *records;
    obmm_export_func_t   obmm_export;
    size_t               lengths[OBMM_EXPORT_BLOCKS_MAX_NUMA_NODES];
    void                *lib;
    unsigned             i;
    int                  ret = 1;

    if (!parse_args(argc, argv, &opts)) {
        usage(argv[0]);
        return 2;
    }

    lib = dlopen(opts.lib_path, RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", opts.lib_path, dlerror());
        return 1;
    }

    obmm_export = (obmm_export_func_t)dlsym(lib, "obmm_export");
    if (obmm_export == NULL) {
        fprintf(stderr, "dlsym(obmm_export) failed: %s\n", dlerror());
        goto out_close_lib;
    }

    records = calloc(opts.count, sizeof(*records));
    if (records == NULL) {
        perror("calloc records");
        goto out_close_lib;
    }

    memset(lengths, 0, sizeof(lengths));
    lengths[opts.numa] = opts.block_size;

    for (i = 0; i < opts.count; ++i) {
        struct export_record     *rec = &records[i];
        struct obmm_mem_desc_dyn *desc;
        unsigned                  block_index = opts.first + i;
        size_t                    priv_len;

        rec->block_index = block_index;
        snprintf(rec->priv, sizeof(rec->priv), "ucx-obmm:%02u", block_index);
        priv_len = strlen(rec->priv);

        desc = calloc(1, sizeof(*desc) + priv_len);
        if (desc == NULL) {
            perror("calloc desc");
            goto out_free_records;
        }

        memcpy(desc->deid, opts.deid, sizeof(desc->deid));
        desc->priv_len = (uint16_t)priv_len;
        memcpy(desc->priv, rec->priv, priv_len);

        errno      = 0;
        rec->memid = obmm_export(lengths, opts.flags, desc);
        if (rec->memid == OBMM_EXPORT_BLOCKS_INVALID_MEMID) {
            fprintf(stderr, "obmm_export(block=%02u, priv=%s) failed: %s\n",
                    block_index, rec->priv, strerror(errno));
            free(desc);
            goto out_free_records;
        }

        rec->desc = desc;
        printf("EXPORTED block=%02u memid=%" PRIu64
               " priv=%s priv_len=%u uba=0x%" PRIx64
               " length=%" PRIu64 " tokenid=%" PRIu32 "\n",
               block_index, rec->memid, rec->priv, desc->priv_len,
               desc->addr, desc->length, desc->tokenid);
    }

    printf("UCX_OBMM_MEMIDS=");
    for (i = 0; i < opts.count; ++i) {
        printf("%s%" PRIu64, (i == 0) ? "" : ",", records[i].memid);
    }
    printf("\n");
    fflush(stdout);
    ret      = 0;

out_free_records:
    for (i = 0; i < opts.count; ++i) {
        free(records[i].desc);
    }
    free(records);
out_close_lib:
    dlclose(lib);
    return ret;
}
