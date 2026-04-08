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

#include <asm-generic/errno.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include <ub/obmm.h>

#include "vendor_adaptor.h"
#include "libobmm.h"

#define NUMA_NO_NODE (-1)
#define OBMM_DEV_PATH "/dev/obmm"

static int obmm_dev_get_fd(void)
{
    static int obmm_dev_fd = -1;
    static pthread_mutex_t obmm_dev_fd_lock = PTHREAD_MUTEX_INITIALIZER;
    int errsv = 0;

    pthread_mutex_lock(&obmm_dev_fd_lock);
    if (obmm_dev_fd < 0) {
        obmm_dev_fd = open(OBMM_DEV_PATH, O_RDWR);
        if (obmm_dev_fd < 0)
            errsv = errno;
    }
    pthread_mutex_unlock(&obmm_dev_fd_lock);
    errno = errsv;
    return obmm_dev_fd;
}

__attribute__((visibility("default"))) int obmm_query_memid_by_pa(unsigned long pa, mem_id *id, unsigned long *offset)
{
    struct obmm_cmd_addr_query cmd_addr_query;
    int fd, ret;

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return fd;

    memset(&cmd_addr_query, 0, sizeof(struct obmm_cmd_addr_query));
    cmd_addr_query.key_type = OBMM_QUERY_BY_PA;
    cmd_addr_query.pa = pa;
    ret = ioctl(fd, OBMM_CMD_ADDR_QUERY, &cmd_addr_query);
    if (ret < 0)
        return ret;

    if (id)
        *id = cmd_addr_query.mem_id;
    if (offset)
        *offset = cmd_addr_query.offset;
    return 0;
}

__attribute__((visibility("default"))) int obmm_query_pa_by_memid(mem_id id, unsigned long offset, unsigned long *pa)
{
    struct obmm_cmd_addr_query cmd_addr_query;
    int fd, ret;

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return fd;
    memset(&cmd_addr_query, 0, sizeof(struct obmm_cmd_addr_query));
    cmd_addr_query.key_type = OBMM_QUERY_BY_ID_OFFSET;
    cmd_addr_query.mem_id = id;
    cmd_addr_query.offset = offset;
    ret = ioctl(fd, OBMM_CMD_ADDR_QUERY, &cmd_addr_query);
    if (ret < 0)
        return ret;

    if (pa)
        *pa = cmd_addr_query.pa;
    return 0;
}

__attribute__((visibility("default"))) mem_id obmm_export_useraddr(int pid, void* va, size_t length,
                unsigned long flags, struct obmm_mem_desc *desc)
{
    struct obmm_cmd_export_pid cmd_export_pid = {0};
    int fd, ret;

    if (desc == NULL) {
        errno = EINVAL;
        return OBMM_INVALID_MEMID;
    }

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return OBMM_INVALID_MEMID;

    cmd_export_pid.va = va;
    cmd_export_pid.length = length;
    cmd_export_pid.pid = pid;
    cmd_export_pid.flags = flags;
    cmd_export_pid.priv_len = desc->priv_len;
    cmd_export_pid.priv = desc->priv;
    memcpy(cmd_export_pid.deid, desc->deid, sizeof(cmd_export_pid.deid));

    ret = vendor_adapt_export(desc, &cmd_export_pid.vendor_info, &cmd_export_pid.vendor_len,
                  &cmd_export_pid.pxm_numa);
    if (ret) {
        errno = ret;
        return OBMM_INVALID_MEMID;
    }
    ret = ioctl(fd, OBMM_CMD_EXPORT_PID, &cmd_export_pid);
    free_vendor_info((void *)cmd_export_pid.vendor_info);
    if (ret < 0)
        return OBMM_INVALID_MEMID;

    desc->addr = cmd_export_pid.uba;
    desc->length = length;
    desc->tokenid = cmd_export_pid.tokenid;
    desc->scna = 0;
    desc->dcna = 0;

    return cmd_export_pid.mem_id;
}

__attribute__((visibility("default"))) mem_id obmm_export(const size_t length[OBMM_MAX_LOCAL_NUMA_NODES],
           unsigned long flags, struct obmm_mem_desc *desc)
{
    struct obmm_cmd_export cmd_export;
    int fd, i, ret, errsv;
    mem_id memid;

    if (length == NULL || desc == NULL) {
        errno = EINVAL;
        return OBMM_INVALID_MEMID;
    }

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return OBMM_INVALID_MEMID;

    memset(&cmd_export, 0, sizeof(struct obmm_cmd_export));
    memcpy(cmd_export.size, length, sizeof(size_t) * OBMM_MAX_LOCAL_NUMA_NODES);
    cmd_export.length = OBMM_MAX_LOCAL_NUMA_NODES;
    cmd_export.flags = flags;
    cmd_export.priv_len = desc->priv_len;
    cmd_export.priv = desc->priv;
    memcpy(cmd_export.deid, desc->deid, sizeof(cmd_export.deid));

    ret = vendor_adapt_export(desc, &cmd_export.vendor_info, &cmd_export.vendor_len, &cmd_export.pxm_numa);
    if (ret) {
        errno = ret;
        return OBMM_INVALID_MEMID;
    }
    ret = ioctl(fd, OBMM_CMD_EXPORT, &cmd_export);
    errsv = errno;
    free_vendor_info((void *)cmd_export.vendor_info);
    errno = errsv;

    if (ret < 0)
        return OBMM_INVALID_MEMID;

    memid = cmd_export.mem_id;

    desc->addr = cmd_export.uba;
    desc->tokenid = cmd_export.tokenid;
    desc->scna = 0;
    desc->dcna = 0;
    desc->length = 0;
    for (i = 0; i < OBMM_MAX_LOCAL_NUMA_NODES; i++)
        desc->length += length[i];

    return memid;
}

static void fill_import_cmd_info(const struct obmm_mem_desc *desc,
                 struct obmm_cmd_import *cmd_import,
                 unsigned long flags, int base_dist)
{
    memset(cmd_import, 0, sizeof(struct obmm_cmd_import));
    cmd_import->addr = desc->addr;
    cmd_import->length = desc->length;
    cmd_import->tokenid = desc->tokenid;
    cmd_import->scna = desc->scna;
    cmd_import->dcna = desc->dcna;
    cmd_import->priv_len = desc->priv_len;
    cmd_import->priv = desc->priv;
    cmd_import->flags = flags;
    cmd_import->base_dist = base_dist;
    memcpy(cmd_import->deid, desc->deid, sizeof(cmd_import->deid));
    memcpy(cmd_import->seid, desc->seid, sizeof(cmd_import->seid));
}

__attribute__((visibility("default"))) mem_id obmm_import(const struct obmm_mem_desc *desc, unsigned long flags,
           int base_dist, int *numa)
{
    struct obmm_cmd_import cmd_import;
    int fd, ret, errsv;
    mem_id memid;

    if (desc == NULL) {
        errno = EINVAL;
        return OBMM_INVALID_MEMID;
    }

    if (((flags & OBMM_IMPORT_FLAG_NUMA_REMOTE) && !(flags & OBMM_IMPORT_FLAG_PREIMPORT)) &&
        (base_dist < 0 || base_dist > UINT8_MAX)) {
        errno = EINVAL;
        return OBMM_INVALID_MEMID;
    }

    fill_import_cmd_info(desc, &cmd_import, flags, base_dist);

    cmd_import.mem_id = 0;
    if (numa != NULL)
        cmd_import.numa_id = *numa;
    else
        cmd_import.numa_id = NUMA_NO_NODE;

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return OBMM_INVALID_MEMID;

    ret = vendor_fixup_import_cmd(&cmd_import);
    if (ret)
        return OBMM_INVALID_MEMID;

    ret = ioctl(fd, OBMM_CMD_IMPORT, &cmd_import);
    errsv = errno;
    vendor_cleanup_import_cmd(&cmd_import);
    errno = errsv;

    if (ret < 0)
        return OBMM_INVALID_MEMID;

    if (numa != NULL)
        *numa = cmd_import.numa_id;
    memid = cmd_import.mem_id;

    return memid;
}

__attribute__((visibility("default"))) int obmm_unexport(mem_id id, unsigned long flags)
{
    struct obmm_cmd_unexport cmd_unexport;
    int fd;

    if (id == OBMM_INVALID_MEMID) {
        errno = EINVAL;
        return -1;
    }

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return fd;

    cmd_unexport.mem_id = id;
    cmd_unexport.flags = flags;

    return ioctl(fd, OBMM_CMD_UNEXPORT, &cmd_unexport);
}

__attribute__((visibility("default"))) int obmm_unimport(mem_id id, unsigned long flags)
{
    struct obmm_cmd_unimport cmd_unimport;
    int fd;

    if (id == OBMM_INVALID_MEMID) {
        errno = EINVAL;
        return -1;
    }

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return fd;

    cmd_unimport.mem_id = id;
    cmd_unimport.flags = flags;

    return ioctl(fd, OBMM_CMD_UNIMPORT, &cmd_unimport);
}

__attribute__((visibility("default"))) int obmm_set_ownership(int fd, void *start, void *end, int prot)
{
    uint64_t mem_attr;
    struct obmm_cmd_update_range update_info;

    if (prot == PROT_NONE) {
        mem_attr = OBMM_SHM_MEM_NORMAL_NC | OBMM_SHM_MEM_NO_ACCESS;
    } else if (prot == PROT_READ) {
        mem_attr = OBMM_SHM_MEM_NORMAL | OBMM_SHM_MEM_READONLY;
    } else if (prot == PROT_WRITE || prot == (PROT_READ | PROT_WRITE)) {
        mem_attr = OBMM_SHM_MEM_NORMAL | OBMM_SHM_MEM_READWRITE;
    } else {
        errno = EINVAL;
        return -1;
    }

    update_info.start = (uintptr_t)start;
    update_info.end = (uintptr_t)end;
    update_info.mem_state = mem_attr;
    update_info.cache_ops = OBMM_SHM_CACHE_INFER;

    return ioctl(fd, OBMM_SHMDEV_UPDATE_RANGE, &update_info);
}

__attribute__((visibility("default"))) int obmm_preimport(struct obmm_preimport_info *preimport_info,
    unsigned long flags)
{
    struct obmm_cmd_preimport cmd;
    int ret, fd, errsv;

    if (preimport_info == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (preimport_info->base_dist < 0 || preimport_info->base_dist > UINT8_MAX) {
        errno = EINVAL;
        return -1;
    }

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return fd;

    cmd.pa = preimport_info->pa;
    cmd.length = preimport_info->length;
    cmd.base_dist = preimport_info->base_dist;
    cmd.numa_id = preimport_info->numa_id;
    cmd.scna = preimport_info->scna;
    cmd.dcna = preimport_info->dcna;
    cmd.priv_len = preimport_info->priv_len;
    cmd.priv = &preimport_info->priv;
    cmd.flags = flags;
    memcpy(cmd.deid, preimport_info->deid, sizeof(cmd.deid));
    memcpy(cmd.seid, preimport_info->seid, sizeof(cmd.seid));

    ret = vendor_fixup_preimport_cmd(&cmd);
    if (ret)
        return ret;

    ret = ioctl(fd, OBMM_CMD_DECLARE_PREIMPORT, &cmd);
    errsv = errno;
    vendor_cleanup_preimport_cmd(&cmd);
    errno = errsv;

    if (ret < 0)
        return ret;
    preimport_info->numa_id = cmd.numa_id;
    return 0;
}

__attribute__((visibility("default"))) int obmm_unpreimport(const struct obmm_preimport_info *preimport_info,
    unsigned long flags)
{
    struct obmm_cmd_preimport cmd;
    int fd;

    if (preimport_info == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = obmm_dev_get_fd();
    if (fd < 0)
        return fd;

    cmd.pa = preimport_info->pa;
    cmd.length = preimport_info->length;
    cmd.base_dist = preimport_info->base_dist;
    cmd.numa_id = preimport_info->numa_id;
    cmd.scna = preimport_info->scna;
    cmd.dcna = preimport_info->dcna;
    cmd.priv_len = preimport_info->priv_len;
    cmd.priv = &preimport_info->priv;
    cmd.flags = flags;
    memcpy(cmd.deid, preimport_info->deid, sizeof(cmd.deid));
    memcpy(cmd.seid, preimport_info->seid, sizeof(cmd.seid));

    return ioctl(fd, OBMM_CMD_UNDECLARE_PREIMPORT, &cmd);
}
