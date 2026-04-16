/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_device.h"

#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/string.h>

#include <glob.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define UCT_OBMM_CPU_SOCKET_GLOB "/sys/devices/system/cpu/cpu*/topology/physical_package_id"


typedef struct uct_obmm_socket_info {
    int           socket_id;
    ucs_cpu_set_t local_cpus;
} uct_obmm_socket_info_t;

static int uct_obmm_read_int_file(const char *path, int *value_p)
{
    FILE *fp;
    char value_str[64];
    char *endp;
    size_t nread;
    long value;

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -errno;
    }

    nread = fread(value_str, 1, sizeof(value_str) - 1, fp);
    fclose(fp);
    if (nread == 0) {
        return -EIO;
    }

    value_str[nread] = '\0';
    errno            = 0;
    value            = strtol(value_str, &endp, 0);
    if ((endp == value_str) || (errno != 0)) {
        return -EINVAL;
    }

    *value_p = (int)value;
    return 0;
}

static int uct_obmm_parse_cpu_id(const char *path)
{
    const char *cpu_token;
    char *endp;
    long cpu;

    cpu_token = strstr(path, "/cpu");
    if (cpu_token == NULL) {
        return -1;
    }

    cpu = strtol(cpu_token + 4, &endp, 10);
    if ((endp == (cpu_token + 4)) || (*endp != '/')) {
        return -1;
    }

    return (int)cpu;
}

static int uct_obmm_socket_info_find(uct_obmm_socket_info_t *sockets,
                                     unsigned num_sockets, int socket_id)
{
    unsigned i;

    for (i = 0; i < num_sockets; ++i) {
        if (sockets[i].socket_id == socket_id) {
            return (int)i;
        }
    }

    return -1;
}

static ucs_status_t
uct_obmm_socket_info_add(uct_obmm_socket_info_t **sockets_p,
                         unsigned *num_sockets_p, int socket_id, unsigned *idx_p)
{
    uct_obmm_socket_info_t *tmp;

    tmp = ucs_realloc(*sockets_p, sizeof(**sockets_p) * (*num_sockets_p + 1),
                      "obmm_socket_infos");
    if (tmp == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    *sockets_p = tmp;
    *idx_p     = *num_sockets_p;
    UCS_CPU_ZERO(&(*sockets_p)[*idx_p].local_cpus);
    (*sockets_p)[*idx_p].socket_id = socket_id;
    ++(*num_sockets_p);
    return UCS_OK;
}

static int uct_obmm_socket_info_compare(const void *arg1, const void *arg2)
{
    const uct_obmm_socket_info_t *s1 = arg1;
    const uct_obmm_socket_info_t *s2 = arg2;

    return (s1->socket_id > s2->socket_id) - (s1->socket_id < s2->socket_id);
}

static ucs_status_t
uct_obmm_discover_sockets(uct_obmm_socket_info_t **sockets_p,
                          unsigned *num_sockets_p)
{
    glob_t g;
    uct_obmm_socket_info_t *sockets;
    unsigned num_sockets, i, idx;
    int cpu, socket_id, rc, find_idx;
    ucs_status_t status;
    ucs_sys_cpuset_t affinity;

    memset(&g, 0, sizeof(g));
    sockets     = NULL;
    num_sockets = 0;

    rc = glob(UCT_OBMM_CPU_SOCKET_GLOB, 0, NULL, &g);
    if ((rc != 0) && (rc != GLOB_NOMATCH)) {
        status = UCS_ERR_IO_ERROR;
        goto out;
    }

    for (i = 0; i < g.gl_pathc; ++i) {
        cpu = uct_obmm_parse_cpu_id(g.gl_pathv[i]);
        if (cpu < 0) {
            continue;
        }

        rc = uct_obmm_read_int_file(g.gl_pathv[i], &socket_id);
        if (rc != 0) {
            continue;
        }

        find_idx = uct_obmm_socket_info_find(sockets, num_sockets, socket_id);
        if (find_idx < 0) {
            status = uct_obmm_socket_info_add(&sockets, &num_sockets, socket_id,
                                              &idx);
            if (status != UCS_OK) {
                goto out;
            }
        } else {
            idx = (unsigned)find_idx;
        }

        if (cpu < UCS_CPU_SETSIZE) {
            UCS_CPU_SET(cpu, &sockets[idx].local_cpus);
        }
    }

    if (num_sockets == 0) {
        status = uct_obmm_socket_info_add(&sockets, &num_sockets, 0, &idx);
        if (status != UCS_OK) {
            goto out;
        }

        if (ucs_sys_getaffinity(&affinity) == 0) {
            ucs_sys_cpuset_copy(&sockets[idx].local_cpus, &affinity);
        }
    }

    qsort(sockets, num_sockets, sizeof(*sockets), uct_obmm_socket_info_compare);
    *sockets_p     = sockets;
    *num_sockets_p = num_sockets;
    status         = UCS_OK;
    sockets        = NULL;

out:
    globfree(&g);
    ucs_free(sockets);
    return status;
}

static int uct_obmm_numa_to_socket(const uct_obmm_socket_info_t *sockets,
                                   unsigned num_sockets, int numa_id)
{
    char path[PATH_MAX];
    char cpulist[64];
    char *endp;
    long cpu;
    FILE *fp;
    unsigned i;
    size_t nread;

    if (numa_id < 0) {
        return -1;
    }

    ucs_snprintf_safe(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist",
                      numa_id);
    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    nread = fread(cpulist, 1, sizeof(cpulist) - 1, fp);
    fclose(fp);
    if (nread == 0) {
        return -1;
    }

    cpulist[nread] = '\0';
    cpu            = strtol(cpulist, &endp, 10);
    if (endp == cpulist) {
        return -1;
    }

    if ((cpu < 0) || (cpu >= UCS_CPU_SETSIZE)) {
        return -1;
    }

    for (i = 0; i < num_sockets; ++i) {
        if (ucs_cpu_is_set((int)cpu, &sockets[i].local_cpus)) {
            return sockets[i].socket_id;
        }
    }

    return -1;
}

static int
uct_obmm_controller_path_to_attr_dir(const char *ctl_path, char *attr_dir,
                                     size_t attr_dir_size)
{
    struct stat st;
    const char *slash;
    size_t len;

    if ((ctl_path == NULL) || (attr_dir == NULL) || (attr_dir_size == 0)) {
        return -EINVAL;
    }

    if ((stat(ctl_path, &st) == 0) && S_ISDIR(st.st_mode)) {
        ucs_strncpy_safe(attr_dir, ctl_path, attr_dir_size);
        return 0;
    }

    slash = strrchr(ctl_path, '/');
    if ((slash == NULL) || (slash == ctl_path)) {
        return -EINVAL;
    }

    len = slash - ctl_path;
    if (len >= attr_dir_size) {
        len = attr_dir_size - 1;
    }

    memcpy(attr_dir, ctl_path, len);
    attr_dir[len] = '\0';
    return 0;
}

static int
uct_obmm_read_controller_attr(const char *attr_dir, const char *attr, int *value_p)
{
    char path[PATH_MAX];

    ucs_snprintf_safe(path, sizeof(path), "%s/%s", attr_dir, attr);
    return uct_obmm_read_int_file(path, value_p);
}

static ucs_status_t
uct_obmm_discover_controllers(uct_obmm_md_t *md,
                              const uct_obmm_socket_info_t *sockets,
                              unsigned num_sockets,
                              uct_obmm_topology_t *topology)
{
    glob_t g;
    uct_obmm_controller_t *controllers;
    unsigned num_controllers, i;
    int eid_value, numa_id, primary_cna, ummu_mapping, socket_id;
    uint32_t eid32;
    int rc;

    memset(&g, 0, sizeof(g));
    controllers    = NULL;
    num_controllers = 0;

    rc = glob(md->config.ctl_glob, 0, NULL, &g);
    if ((rc != 0) || (g.gl_pathc == 0)) {
        topology->controllers     = NULL;
        topology->num_controllers = 0;
        globfree(&g);
        return UCS_OK;
    }

    controllers = ucs_calloc(g.gl_pathc, sizeof(*controllers),
                             "obmm_controllers");
    if (controllers == NULL) {
        globfree(&g);
        return UCS_ERR_NO_MEMORY;
    }

    for (i = 0; i < g.gl_pathc; ++i) {
        char attr_dir[PATH_MAX];

        if (uct_obmm_controller_path_to_attr_dir(g.gl_pathv[i], attr_dir,
                                                 sizeof(attr_dir)) != 0) {
            continue;
        }

        if (uct_obmm_read_controller_attr(attr_dir, "eid", &eid_value) != 0) {
            continue;
        }
        if (uct_obmm_read_controller_attr(attr_dir, "numa", &numa_id) != 0) {
            continue;
        }
        if (uct_obmm_read_controller_attr(attr_dir, "primary_cna",
                                          &primary_cna) != 0) {
            continue;
        }
        if (uct_obmm_read_controller_attr(attr_dir, "ummu_map",
                                          &ummu_mapping) != 0) {
            continue;
        }

        socket_id = uct_obmm_numa_to_socket(sockets, num_sockets, numa_id);
        if ((socket_id < 0) && (num_sockets > 0)) {
            socket_id = sockets[0].socket_id;
        }

        memset(&controllers[num_controllers], 0, sizeof(*controllers));
        eid32 = (uint32_t)eid_value;
        memcpy(controllers[num_controllers].eid, &eid32, sizeof(eid32));
        controllers[num_controllers].numa_id      = numa_id;
        controllers[num_controllers].socket_id    = socket_id;
        controllers[num_controllers].primary_cna  = (uint32_t)primary_cna;
        controllers[num_controllers].ummu_mapping = ummu_mapping;
        ucs_strncpy_safe(controllers[num_controllers].sysfs_path, attr_dir,
                         sizeof(controllers[num_controllers].sysfs_path));
        ++num_controllers;
    }

    if (num_controllers == 0) {
        ucs_free(controllers);
        controllers = NULL;
    }

    topology->controllers      = controllers;
    topology->num_controllers  = num_controllers;
    globfree(&g);
    return UCS_OK;
}

static uct_obmm_controller_t *
uct_obmm_choose_primary_controller(const uct_obmm_topology_t *topology,
                                   int socket_id, unsigned *num_controllers_p)
{
    uct_obmm_controller_t *primary;
    unsigned i, num_controllers;

    primary         = NULL;
    num_controllers = 0;

    for (i = 0; i < topology->num_controllers; ++i) {
        if (topology->controllers[i].socket_id != socket_id) {
            continue;
        }

        ++num_controllers;
        if ((primary == NULL) ||
            (topology->controllers[i].ummu_mapping < primary->ummu_mapping)) {
            primary = &topology->controllers[i];
        }
    }

    *num_controllers_p = num_controllers;
    return primary;
}

static void uct_obmm_device_init_caps(uct_obmm_device_t *device, size_t granularity)
{
    device->caps.type             = UCT_DEVICE_TYPE_SHM;
    device->caps.sys_device       = UCS_SYS_DEVICE_ID_UNKNOWN;
    device->caps.dedicated_bw     = UCT_OBMM_DEFAULT_BW;
    device->caps.shared_bw        = 0;
    device->caps.latency          = ucs_linear_func_make(UCT_OBMM_DEFAULT_LATENCY, 0);
    device->caps.overhead         = UCT_OBMM_DEFAULT_OVERHEAD;
    device->caps.reg_alignment    = granularity;
    device->caps.obmm_granularity = granularity;
    device->caps.iface_flags      = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                    UCT_IFACE_FLAG_CB_SYNC |
                                    UCT_IFACE_FLAG_EP_CHECK;
    device->caps.md_flags         = UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_RKEY;
}

static ucs_status_t
uct_obmm_build_devices(uct_obmm_md_t *md, const uct_obmm_socket_info_t *sockets,
                       unsigned num_sockets, uct_obmm_topology_t *topology)
{
    uct_obmm_device_t *devices;
    uct_obmm_device_t *device;
    unsigned i;
    ucs_status_t status;

    devices = ucs_calloc(num_sockets, sizeof(*devices), "obmm_devices");
    if (devices == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    for (i = 0; i < num_sockets; ++i) {
        device = &devices[i];
        memset(device, 0, sizeof(*device));

        ucs_snprintf_zero(device->name, sizeof(device->name), "obmm_sock%d",
                          sockets[i].socket_id);
        device->index      = i;
        device->socket_id  = sockets[i].socket_id;
        device->numa_id    = -1;
        device->local_cpus = sockets[i].local_cpus;
        device->generation = md->generation;
        uct_obmm_device_init_caps(device, topology->obmm_granularity);

        status = ucs_spinlock_init(&device->lock, 0);
        if (status != UCS_OK) {
            goto err;
        }

        device->primary_ctl = uct_obmm_choose_primary_controller(
                topology, device->socket_id, &device->num_controllers);
        if (device->primary_ctl == NULL) {
            device->flags |= UCT_OBMM_DEVICE_FLAG_DEGRADED;
            continue;
        }

        device->numa_id    = device->primary_ctl->numa_id;
        device->res_domain = ucs_calloc(1, sizeof(*device->res_domain),
                                        "obmm_res_domain");
        if (device->res_domain == NULL) {
            device->flags |= UCT_OBMM_DEVICE_FLAG_DEGRADED;
            continue;
        }

        status = uct_obmm_resource_domain_init(device->res_domain, &md->config);
        if (status != UCS_OK) {
            ucs_free(device->res_domain);
            device->res_domain = NULL;
            device->flags     |= UCT_OBMM_DEVICE_FLAG_DEGRADED;
        }
    }

    topology->devices     = devices;
    topology->num_devices = num_sockets;
    return UCS_OK;

err:
    while (i > 0) {
        --i;
        if (devices[i].res_domain != NULL) {
            uct_obmm_resource_domain_cleanup(devices[i].res_domain);
            ucs_free(devices[i].res_domain);
        }
        ucs_spinlock_destroy(&devices[i].lock);
    }
    ucs_free(devices);
    return status;
}

ucs_status_t uct_obmm_topology_discover(uct_obmm_md_t *md,
                                        uct_obmm_topology_t *topology)
{
    uct_obmm_socket_info_t *sockets;
    unsigned num_sockets;
    ucs_status_t status;

    memset(topology, 0, sizeof(*topology));
    topology->obmm_granularity = (md->config.granularity == 0) ?
                                 UCT_OBMM_DEFAULT_GRANULARITY :
                                 md->config.granularity;
    topology->generation       = md->generation;

    sockets     = NULL;
    num_sockets = 0;

    status = uct_obmm_discover_sockets(&sockets, &num_sockets);
    if (status != UCS_OK) {
        goto out;
    }

    status = uct_obmm_discover_controllers(md, sockets, num_sockets, topology);
    if (status != UCS_OK) {
        goto out_free_sockets;
    }

    status = uct_obmm_build_devices(md, sockets, num_sockets, topology);
    if (status != UCS_OK) {
        goto out_cleanup_topology;
    }

    ucs_free(sockets);
    return UCS_OK;

out_cleanup_topology:
    uct_obmm_topology_cleanup(topology);
out_free_sockets:
    ucs_free(sockets);
out:
    return status;
}

void uct_obmm_topology_cleanup(uct_obmm_topology_t *topology)
{
    uct_obmm_device_t *device;
    unsigned i;

    if (topology == NULL) {
        return;
    }

    if (topology->devices != NULL) {
        for (i = 0; i < topology->num_devices; ++i) {
            device = &topology->devices[i];
            if (device->res_domain != NULL) {
                uct_obmm_resource_domain_cleanup(device->res_domain);
                ucs_free(device->res_domain);
            }
            ucs_spinlock_destroy(&device->lock);
        }
        ucs_free(topology->devices);
    }

    ucs_free(topology->controllers);
    memset(topology, 0, sizeof(*topology));
}

ucs_status_t uct_obmm_device_catalog_lookup(uct_obmm_md_t *md,
                                            const char *dev_name,
                                            uct_obmm_device_t **device_p)
{
    unsigned i;

    if ((md == NULL) || (dev_name == NULL) || (device_p == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    for (i = 0; i < md->topology.num_devices; ++i) {
        if (strcmp(md->topology.devices[i].name, dev_name)) {
            continue;
        }

        if ((md->topology.devices[i].flags & UCT_OBMM_DEVICE_FLAG_DEGRADED) ||
            (md->topology.devices[i].primary_ctl == NULL) ||
            (md->topology.devices[i].res_domain == NULL)) {
            return UCS_ERR_NO_DEVICE;
        }

        *device_p = &md->topology.devices[i];
        return UCS_OK;
    }

    return UCS_ERR_NO_DEVICE;
}
