/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * libobmm is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 *
 * Description: libobmm main api
 * Author: Gao Chao
 * Create: 2025-10-28
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>

#include <libobmm.h>
#include "vendor_adaptor.h"

#define pr_err(fmt, ...)	fprintf(stderr, "libobmm: [vendor-adaptor][ERROR]" fmt, ##__VA_ARGS__)

#define EID_FMT64 "%#lx:%#lx"
#define EID_ARGS64(eid) (*(uint64_t *)&(eid)[8]), (*(uint64_t *)&(eid)[0])

#define EID_SIZE 16
#define MAX_CONTROLLERS 8
#define MAX_PATH 256
#define MAX_CHAR 64
#define INVAL_UMMU_MAPPING (-1)

enum hisi_ummu_tdev_version {
    HISI_TDEV_INFO_V1 = 0,
};

struct hisi_ummu_tdev_info {
    enum hisi_ummu_tdev_version ver;
    union {
        struct {
            unsigned long ummu_idx_mask; // ummu_mapping mask
            bool on_chip; // sram / dram
        } v1;
    };
};

struct ub_bus_ctl_node {
    int ummu_mapping;
    int numa_id;
    bool valid;
};

static uint8_t g_invalid_eid[16];

static int read_int_from_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    char str[MAX_CHAR], *end;
    size_t nread;
    long ret;

    if (!fp) {
        pr_err("failed to open file %s.\n", path);
        return -1;
    }

    nread = fread(str, 1, sizeof(str) - 1, fp);
    if (nread == 0) {
        pr_err("failed to read data from %s.\n", path);
        (void)fclose(fp);
        return -1;
    }
    (void)fclose(fp);
    str[nread] = '\0';
    /* hex and decimal are possible */
    ret = strtol(str, &end, 0);
    if (end == str) {
        pr_err("failed to parse int value from '%s' in %s.\n", str, path);
        return -1;
    }
    if (ret > INT_MAX || ret < INT_MIN) {
        pr_err("read occured overflowed %s.\n", path);
        return -1;
    }
    return (int)ret;
}

static int get_ubc_attr(const char *ubc_path, const char *attr)
{
    char attr_path[MAX_PATH];
    int ret;

    ret = snprintf(attr_path, sizeof(attr_path), "%s/%s", ubc_path, attr);
    if (ret <= 0)
        return -1;
    return read_int_from_file(attr_path);
}

static int get_ubc_path(int ubc_index, char *ubc_path, size_t path_len)
{
    char pattern[MAX_PATH], *glob_path;
    glob_t g;
    int ret;

    (void)snprintf(pattern, sizeof(pattern), "/sys/devices/ub_bus_controller%d/*/ubc", ubc_index);

    ret = glob(pattern, 0, NULL, &g);
    if (ret != 0) {
        globfree(&g);
        return ENODEV;
    }
    if (g.gl_pathc == 0) {
        globfree(&g);
        return ENODEV;
    }
    glob_path = dirname(g.gl_pathv[0]);
    if (strlen(glob_path) >= path_len) {
        globfree(&g);
        return EINVAL;
    }
    (void)snprintf(ubc_path, path_len, "%s", glob_path);
    globfree(&g);
    return 0;
}

static int get_ubc_by_eid(unsigned int *uba_index, char *ubc_path, size_t path_len, const uint8_t *eid)
{
    for (unsigned int i = 0; i < MAX_CONTROLLERS; i++) {
        int ret = get_ubc_path(i, ubc_path, path_len);
        if (ret)
            continue;

        ret = get_ubc_attr(ubc_path, "eid"); /* host endian */
        if (ret < 0) {
            pr_err("failed to read ctl eid, path %s.\n", ubc_path);
            errno = ENODEV;
            return -1;
        }

        uint8_t sysfs_eid[EID_SIZE] = {};
        *(unsigned int*)sysfs_eid = (unsigned int)ret;

        if (memcmp(sysfs_eid, eid, EID_SIZE) != 0)
            continue;

        *uba_index = i;
        return 0;
    }
    pr_err("failed to find ctl, eid:" EID_FMT64 ".\n", EID_ARGS64(eid));
    errno = ENODEV;
    return -1;
}

static struct ub_bus_ctl_node get_ctl_by_eid(uint8_t *eid)
{
    struct ub_bus_ctl_node node = {0};
    char ubc_path[MAX_PATH];
    unsigned int ubc_index;

    int ret = get_ubc_by_eid(&ubc_index, ubc_path, sizeof(ubc_path), eid);
    if (ret)
        return node;

    node.ummu_mapping = get_ubc_attr(ubc_path, "ummu_map");
    if (node.ummu_mapping < 0) {
        pr_err("failed to read ctl ummu_map, path %s.\n", ubc_path);
        return node;
    }

    node.numa_id = get_ubc_attr(ubc_path, "numa");
    if (node.numa_id < 0) {
        pr_err("failed to read ctl numa, path %s.\n", ubc_path);
        return node;
    }
    node.valid = true;
    return node;
}

static int get_primary_cna_by_eid(unsigned int *cna, const uint8_t *eid)
{
    char ubc_path[MAX_PATH];
    unsigned int ubc_index;

    int ret = get_ubc_by_eid(&ubc_index, ubc_path, sizeof(ubc_path), eid);
    if (ret)
        return ret;

    ret = get_ubc_attr(ubc_path, "primary_cna");
    if (ret < 0) {
        pr_err("failed to read ctl primary_cna, path %s.\n", ubc_path);
        errno = ENODEV;
        return -1;
    }
    *cna = (unsigned int)ret;

    return 0;
}

static int init_vendor_info(int ummu_mapping, const void **vendor_info, uint16_t *vendor_len)
{
    struct hisi_ummu_tdev_info *info = (struct hisi_ummu_tdev_info *)calloc(1, sizeof(*info));

    if (!info)
        return ENOMEM;

    if (sizeof(struct hisi_ummu_tdev_info) > OBMM_MAX_VENDOR_LEN) {
        free(info);
        return EINVAL;
    }

    info->ver = HISI_TDEV_INFO_V1;
    info->v1.on_chip = true;
    info->v1.ummu_idx_mask = 1 << ummu_mapping;
    *vendor_info = info;
    *vendor_len = sizeof(struct hisi_ummu_tdev_info);
    return 0;
}

int vendor_adapt_export(struct obmm_mem_desc *desc, const void **vendor_info,
            uint16_t *vendor_len, int *numa)
{
    struct ub_bus_ctl_node node;
    int ret;

    if (memcmp(desc->deid, g_invalid_eid, sizeof(desc->deid)) == 0) {
        pr_err("zero-type eid is not allowed.\n");
        return EINVAL;
    }
    node = get_ctl_by_eid(desc->deid);
    if (!node.valid)
        return ENODEV;

    ret = init_vendor_info(node.ummu_mapping, vendor_info, vendor_len);
    if (ret) {
        pr_err("init_vendor_info failed, ret %d.\n", ret);
        return ret;
    }
    *numa = node.numa_id;
    return 0;
}

void free_vendor_info(void *vendor_info)
{
    free(vendor_info);
}

int vendor_fixup_import_cmd(struct obmm_cmd_import *cmd)
{
    unsigned int cna;
    int ret = get_primary_cna_by_eid(&cna, cmd->seid);
    if (ret)
        return ret;
    if (cna != cmd->scna) {
        pr_err("ctl with eid " EID_FMT64 " has scna=%#x which is different from scna=%#x.\n",
                EID_ARGS64(cmd->seid), cna, cmd->scna);
        errno = ENODEV;
        return -1;
    }
    return 0;
}

void vendor_cleanup_import_cmd(struct obmm_cmd_import *cmd)
{
}

int vendor_fixup_preimport_cmd(struct obmm_cmd_preimport *cmd)
{
    unsigned int cna;
    int ret = get_primary_cna_by_eid(&cna, cmd->seid);
    if (ret)
        return ret;
    if (cna != cmd->scna) {
        pr_err("ctl with eid " EID_FMT64 " has scna=%#x which is different from scna=%#x.\n",
                EID_ARGS64(cmd->seid), cna, cmd->scna);
        errno = ENODEV;
        return -1;
    }
    return 0;
}

void vendor_cleanup_preimport_cmd(struct obmm_cmd_preimport *cmd)
{
}
