/*
 * Standalone OBMM import helper for the block-FIFO UCX layout.
 *
 * This file intentionally does not include libobmm headers and does not link
 * libobmm at build time. It loads libobmm.so at runtime and resolves only
 * obmm_import.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBMM_IMPORT_BLOCKS_DEFAULT_SIZE       (34ull * 1024ull * 1024ull)
#define OBMM_IMPORT_BLOCKS_DEFAULT_FLAGS      1ul
#define OBMM_IMPORT_BLOCKS_INVALID_MEMID      0ull

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

typedef obmm_mem_id_t (*obmm_import_func_t)(
        const struct obmm_mem_desc_dyn *desc, unsigned long flags,
        int base_dist, int *numa);

struct import_record {
    obmm_mem_id_t             memid;
    struct obmm_mem_desc_dyn *desc;
    uint64_t                  pa;
    unsigned                  block_index;
    char                      priv[16];
};

struct options {
    const char   *lib_path;
    const char   *pa_file;
    unsigned long flags;
    unsigned      first;
    size_t        block_size;
    int           base_dist;
    uint8_t       seid[16];
    uint8_t       remote_deid[16];
    uint32_t      scna;
    uint32_t      remote_dcna;
    int           seid_set;
    int           remote_deid_set;
    int           scna_set;
    int           remote_dcna_set;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --pa-file FILE --seid HEX32 --scna CNA "
            "--remote-deid HEX32 --remote-dcna CNA [options]\n"
            "\n"
            "Options:\n"
            "  --lib PATH        libobmm shared object path "
            "(default: libobmm.so)\n"
            "  --flags FLAGS     numeric obmm_import flags "
            "(default: 1, expected ALLOW_MMAP on current target)\n"
            "  --first N         first block index for one-column PA files "
            "(default: 0)\n"
            "  --size BYTES      bytes per import block (default: 34MiB)\n"
            "  --base-dist N     obmm_import base_dist (default: 0)\n"
            "  --help            show this help\n"
            "\n"
            "PA file format:\n"
            "  Blank lines and lines starting with # are ignored.\n"
            "  One token: PA, assigned to block indexes --first, --first+1, ...\n"
            "  Two tokens: BLOCK_INDEX PA, explicitly selecting ucx-obmm:NN.\n",
            prog);
}

static int parse_ulong(const char *arg, unsigned long *value_p)
{
    char *end = NULL;

    errno     = 0;
    *value_p = strtoul(arg, &end, 0);
    return (errno == 0) && (end != arg) && (*end == '\0');
}

static int parse_uint32(const char *arg, uint32_t *value_p)
{
    unsigned long value;

    if (!parse_ulong(arg, &value) || (value > UINT32_MAX)) {
        return 0;
    }

    *value_p = (uint32_t)value;
    return 1;
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

static int parse_int(const char *arg, int *value_p)
{
    char *end = NULL;
    long  value;

    errno = 0;
    value = strtol(arg, &end, 0);
    if ((errno != 0) || (end == arg) || (*end != '\0') ||
        (value < INT32_MIN) || (value > INT32_MAX)) {
        return 0;
    }

    *value_p = (int)value;
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
    opts->flags      = OBMM_IMPORT_BLOCKS_DEFAULT_FLAGS;
    opts->first      = 0;
    opts->block_size = (size_t)OBMM_IMPORT_BLOCKS_DEFAULT_SIZE;
    opts->base_dist  = 0;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "--lib") && (++i < argc)) {
            opts->lib_path = argv[i];
        } else if (!strcmp(argv[i], "--pa-file") && (++i < argc)) {
            opts->pa_file = argv[i];
        } else if (!strcmp(argv[i], "--flags") && (++i < argc)) {
            if (!parse_ulong(argv[i], &opts->flags)) {
                fprintf(stderr, "invalid --flags value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--first") && (++i < argc)) {
            if (!parse_uint(argv[i], &opts->first)) {
                fprintf(stderr, "invalid --first value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--size") && (++i < argc)) {
            if (!parse_size(argv[i], &opts->block_size)) {
                fprintf(stderr, "invalid --size value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--base-dist") && (++i < argc)) {
            if (!parse_int(argv[i], &opts->base_dist)) {
                fprintf(stderr, "invalid --base-dist value: %s\n", argv[i]);
                return 0;
            }
        } else if (!strcmp(argv[i], "--seid") && (++i < argc)) {
            if (!parse_hex16(argv[i], opts->seid)) {
                fprintf(stderr, "invalid --seid value: %s\n", argv[i]);
                return 0;
            }
            opts->seid_set = 1;
        } else if (!strcmp(argv[i], "--remote-deid") && (++i < argc)) {
            if (!parse_hex16(argv[i], opts->remote_deid)) {
                fprintf(stderr, "invalid --remote-deid value: %s\n", argv[i]);
                return 0;
            }
            opts->remote_deid_set = 1;
        } else if (!strcmp(argv[i], "--scna") && (++i < argc)) {
            if (!parse_uint32(argv[i], &opts->scna)) {
                fprintf(stderr, "invalid --scna value: %s\n", argv[i]);
                return 0;
            }
            opts->scna_set = 1;
        } else if (!strcmp(argv[i], "--remote-dcna") && (++i < argc)) {
            if (!parse_uint32(argv[i], &opts->remote_dcna)) {
                fprintf(stderr, "invalid --remote-dcna value: %s\n",
                        argv[i]);
                return 0;
            }
            opts->remote_dcna_set = 1;
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            return 0;
        }
    }

    if (opts->pa_file == NULL) {
        fprintf(stderr, "--pa-file is required\n");
        return 0;
    }
    if (!opts->seid_set) {
        fprintf(stderr, "--seid is required\n");
        return 0;
    }
    if (!opts->scna_set) {
        fprintf(stderr, "--scna is required\n");
        return 0;
    }
    if (!opts->remote_deid_set) {
        fprintf(stderr, "--remote-deid is required\n");
        return 0;
    }
    if (!opts->remote_dcna_set) {
        fprintf(stderr, "--remote-dcna is required\n");
        return 0;
    }
    if ((opts->first > 99) || (opts->block_size == 0)) {
        fprintf(stderr, "invalid --first or --size\n");
        return 0;
    }

    return 1;
}

static char *trim_line(char *line)
{
    char *end;

    while (isspace((unsigned char)*line)) {
        ++line;
    }

    end = line + strlen(line);
    while ((end > line) && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }

    return line;
}

static int read_pa_file(const char *path, unsigned first,
                        struct import_record **records_p, unsigned *count_p)
{
    struct import_record *records = NULL;
    unsigned              count   = 0;
    unsigned              cap     = 0;
    unsigned              seq     = 0;
    FILE                 *f;
    char                  line[256];
    unsigned              line_no = 0;
    int                   used_blocks[100] = { 0 };

    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "failed to open PA file %s: %s\n", path,
                strerror(errno));
        return 0;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        struct import_record *rec;
        char                 *cursor;
        char                 *tok0;
        char                 *tok1;
        char                 *tok2;
        char                 *comment;
        unsigned long         pa;
        unsigned              block_index;

        ++line_no;
        cursor = trim_line(line);
        comment = strchr(cursor, '#');
        if (comment != NULL) {
            *comment = '\0';
            cursor = trim_line(cursor);
        }
        if ((*cursor == '\0') || (*cursor == '#')) {
            continue;
        }

        tok0 = strtok(cursor, " \t");
        tok1 = strtok(NULL, " \t");
        tok2 = strtok(NULL, " \t");
        if ((tok0 == NULL) || (tok2 != NULL)) {
            fprintf(stderr, "%s:%u: expected PA or BLOCK_INDEX PA\n", path,
                    line_no);
            goto err;
        }

        if (tok1 == NULL) {
            block_index = first + seq;
            if (!parse_ulong(tok0, &pa)) {
                fprintf(stderr, "%s:%u: invalid PA: %s\n", path, line_no,
                        tok0);
                goto err;
            }
        } else {
            if (!parse_uint(tok0, &block_index)) {
                fprintf(stderr, "%s:%u: invalid block index: %s\n", path,
                        line_no, tok0);
                goto err;
            }
            if (!parse_ulong(tok1, &pa)) {
                fprintf(stderr, "%s:%u: invalid PA: %s\n", path, line_no,
                        tok1);
                goto err;
            }
        }

        if (block_index > 99) {
            fprintf(stderr, "%s:%u: block index %u does not fit "
                            "ucx-obmm:NN\n",
                    path, line_no, block_index);
            goto err;
        }
        if (used_blocks[block_index]) {
            fprintf(stderr, "%s:%u: duplicate block index %u\n", path,
                    line_no, block_index);
            goto err;
        }
        if (pa == 0) {
            fprintf(stderr, "%s:%u: PA must be nonzero\n", path, line_no);
            goto err;
        }

        if (count == cap) {
            unsigned new_cap = (cap == 0) ? 16 : (cap * 2);
            void    *new_records;

            new_records = realloc(records, new_cap * sizeof(*records));
            if (new_records == NULL) {
                perror("realloc records");
                goto err;
            }
            records = new_records;
            memset(records + cap, 0, (new_cap - cap) * sizeof(*records));
            cap = new_cap;
        }

        rec              = &records[count++];
        rec->block_index = block_index;
        rec->pa          = pa;
        used_blocks[block_index] = 1;
        ++seq;
    }

    if (ferror(f)) {
        fprintf(stderr, "failed to read PA file %s: %s\n", path,
                strerror(errno));
        goto err;
    }

    fclose(f);
    *records_p = records;
    *count_p   = count;
    return 1;

err:
    fclose(f);
    free(records);
    return 0;
}

int main(int argc, char **argv)
{
    struct options       opts;
    struct import_record *records = NULL;
    obmm_import_func_t   obmm_import;
    void                *lib;
    unsigned             i, count = 0;
    int                  ret = 1;

    if (!parse_args(argc, argv, &opts)) {
        usage(argv[0]);
        return 2;
    }

    if (!read_pa_file(opts.pa_file, opts.first, &records, &count)) {
        return 1;
    }
    if (count == 0) {
        fprintf(stderr, "PA file has no import entries\n");
        free(records);
        return 1;
    }

    lib = dlopen(opts.lib_path, RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", opts.lib_path, dlerror());
        goto out_free_records;
    }

    obmm_import = (obmm_import_func_t)dlsym(lib, "obmm_import");
    if (obmm_import == NULL) {
        fprintf(stderr, "dlsym(obmm_import) failed: %s\n", dlerror());
        goto out_close_lib;
    }

    for (i = 0; i < count; ++i) {
        struct import_record     *rec = &records[i];
        struct obmm_mem_desc_dyn *desc;
        size_t                    priv_len;

        snprintf(rec->priv, sizeof(rec->priv), "ucx-obmm:%02u",
                 rec->block_index);
        priv_len = strlen(rec->priv);

        desc = calloc(1, sizeof(*desc) + priv_len);
        if (desc == NULL) {
            perror("calloc desc");
            goto out_close_lib;
        }

        desc->addr     = rec->pa;
        desc->length   = opts.block_size;
        desc->scna     = opts.scna;
        desc->dcna     = opts.remote_dcna;
        desc->priv_len = (uint16_t)priv_len;
        memcpy(desc->seid, opts.seid, sizeof(desc->seid));
        memcpy(desc->deid, opts.remote_deid, sizeof(desc->deid));
        memcpy(desc->priv, rec->priv, priv_len);

        errno      = 0;
        rec->memid = obmm_import(desc, opts.flags, opts.base_dist, NULL);
        if (rec->memid == OBMM_IMPORT_BLOCKS_INVALID_MEMID) {
            fprintf(stderr, "obmm_import(block=%02u, pa=0x%" PRIx64
                            ", priv=%s) failed: %s\n",
                    rec->block_index, rec->pa, rec->priv, strerror(errno));
            free(desc);
            goto out_close_lib;
        }

        rec->desc = desc;
        printf("IMPORTED block=%02u memid=%" PRIu64
               " pa=0x%" PRIx64 " length=%" PRIu64
               " priv=%s priv_len=%u\n",
               rec->block_index, rec->memid, rec->pa, desc->length, rec->priv,
               desc->priv_len);
    }

    printf("UCX_OBMM_IMPORT_MEMIDS=");
    for (i = 0; i < count; ++i) {
        printf("%s%" PRIu64, (i == 0) ? "" : ",", records[i].memid);
    }
    printf("\n");
    fflush(stdout);
    ret = 0;

out_close_lib:
    dlclose(lib);
out_free_records:
    for (i = 0; i < count; ++i) {
        free(records[i].desc);
    }
    free(records);
    return ret;
}
