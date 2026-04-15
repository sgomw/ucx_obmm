/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_resource.h"

#include <ucs/debug/log.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>
#include <ucs/sys/string.h>
#include <ucs/type/init_once.h>

#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


#if defined(__has_include)
#  if __has_include(<libobmm.h>)
#    include <libobmm.h>
#    define UCT_OBMM_HAVE_LIBOBMM_H 1
#  endif
#endif

#ifndef UCT_OBMM_HAVE_LIBOBMM_H
typedef uint64_t mem_id;

struct obmm_mem_desc {
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

struct obmm_preimport_info {
    uint64_t pa;
    uint64_t length;
    int      base_dist;
    int      numa_id;
    uint8_t  seid[16];
    uint8_t  deid[16];
    uint32_t scna;
    uint32_t dcna;
    uint16_t priv_len;
    uint8_t  priv[];
};
#endif

#ifndef OBMM_INVALID_MEMID
#define OBMM_INVALID_MEMID 0
#endif

#ifndef OBMM_MAX_PRIV_LEN
#define OBMM_MAX_PRIV_LEN UCT_OBMM_MAX_PRIV_LEN
#endif

#ifndef OBMM_EXPORT_FLAG_ALLOW_MMAP
#define OBMM_EXPORT_FLAG_ALLOW_MMAP 0x1UL
#endif

#ifndef OBMM_IMPORT_FLAG_ALLOW_MMAP
#define OBMM_IMPORT_FLAG_ALLOW_MMAP 0x1UL
#endif

#ifndef OBMM_IMPORT_FLAG_PREIMPORT
#define OBMM_IMPORT_FLAG_PREIMPORT 0x2UL
#endif

#ifndef OBMM_IMPORT_FLAG_NUMA_REMOTE
#define OBMM_IMPORT_FLAG_NUMA_REMOTE 0x4UL
#endif


typedef struct uct_obmm_api {
    int   ready;
    void *dl_handle;
    mem_id (*obmm_export_useraddr)(int pid, void *va, size_t length,
                                   unsigned long flags,
                                   struct obmm_mem_desc *desc);
    int (*obmm_unexport)(mem_id id, unsigned long flags);
    mem_id (*obmm_import)(const struct obmm_mem_desc *desc, unsigned long flags,
                          int base_dist, int *numa);
    int (*obmm_unimport)(mem_id id, unsigned long flags);
    int (*obmm_preimport)(struct obmm_preimport_info *preimport_info,
                          unsigned long flags);
    int (*obmm_unpreimport)(const struct obmm_preimport_info *preimport_info,
                            unsigned long flags);
} uct_obmm_api_t;


static uct_obmm_api_t uct_obmm_api;
static ucs_init_once_t uct_obmm_api_init_once = UCS_INIT_ONCE_INITIALIZER;


static void uct_obmm_api_init()
{
    static const char *const libs[] = {"libobmm.so", "libobmm.so.1"};
    unsigned i;

    memset(&uct_obmm_api, 0, sizeof(uct_obmm_api));

    for (i = 0; i < (sizeof(libs) / sizeof(libs[0])); ++i) {
        uct_obmm_api.dl_handle = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
        if (uct_obmm_api.dl_handle != NULL) {
            break;
        }
    }

    if (uct_obmm_api.dl_handle == NULL) {
        ucs_debug("failed to load libobmm.so: %s", dlerror());
        return;
    }

#define UCT_OBMM_DLSYM(_name) \
    do { \
        void *sym = dlsym(uct_obmm_api.dl_handle, #_name); \
        if (sym == NULL) { \
            ucs_error("failed to resolve symbol %s in libobmm: %s", #_name, \
                      dlerror()); \
            goto err_close; \
        } \
        *(void**)(&uct_obmm_api._name) = sym; \
    } while (0)

    UCT_OBMM_DLSYM(obmm_export_useraddr);
    UCT_OBMM_DLSYM(obmm_unexport);
    UCT_OBMM_DLSYM(obmm_import);
    UCT_OBMM_DLSYM(obmm_unimport);
    UCT_OBMM_DLSYM(obmm_preimport);
    UCT_OBMM_DLSYM(obmm_unpreimport);
    uct_obmm_api.ready = 1;
    return;

err_close:
    dlclose(uct_obmm_api.dl_handle);
    memset(&uct_obmm_api, 0, sizeof(uct_obmm_api));
#undef UCT_OBMM_DLSYM
}

static int uct_obmm_api_is_ready()
{
    UCS_INIT_ONCE(&uct_obmm_api_init_once) {
        uct_obmm_api_init();
    }

    return uct_obmm_api.ready;
}

static uint64_t uct_obmm_preimport_key(uint64_t remote_addr, size_t length)
{
    return (remote_addr >> 12) ^ ((uint64_t)length << 1);
}

static int uct_obmm_is_device_usable(uct_obmm_device_t *device)
{
    return (device != NULL) && (device->primary_ctl != NULL) &&
           (device->res_domain != NULL) &&
           !(device->flags & UCT_OBMM_DEVICE_FLAG_DEGRADED);
}

static int uct_obmm_is_aligned(uintptr_t address, size_t length, size_t alignment)
{
    if (alignment <= 1) {
        return 1;
    }

    return ((address % alignment) == 0) && ((length % alignment) == 0);
}

static uct_obmm_device_t *uct_obmm_md_get_default_device(uct_obmm_md_t *md)
{
    uct_obmm_device_t *device;
    unsigned i;

    if (md->config.default_socket != UCT_OBMM_DEFAULT_SOCKET_AUTO) {
        for (i = 0; i < md->topology.num_devices; ++i) {
            device = &md->topology.devices[i];
            if ((device->socket_id == md->config.default_socket) &&
                uct_obmm_is_device_usable(device)) {
                return device;
            }
        }
    }

    for (i = 0; i < md->topology.num_devices; ++i) {
        device = &md->topology.devices[i];
        if (uct_obmm_is_device_usable(device)) {
            return device;
        }
    }

    return NULL;
}

static uct_obmm_device_t *uct_obmm_md_select_device(uct_obmm_md_t *md)
{
    int cpu, i;
    uct_obmm_device_t *device;

    cpu = ucs_get_first_cpu();
    if ((cpu >= 0) && (cpu < UCS_CPU_SETSIZE)) {
        for (i = 0; i < (int)md->topology.num_devices; ++i) {
            device = &md->topology.devices[i];
            if (uct_obmm_is_device_usable(device) &&
                ucs_cpu_is_set(cpu, &device->local_cpus)) {
                return device;
            }
        }
    }

    return uct_obmm_md_get_default_device(md);
}

static ucs_status_t
uct_obmm_domain_reserve_export(uct_obmm_resource_domain_t *domain, size_t length)
{
    ucs_status_t status;

    ucs_spin_lock(&domain->lock);
    if ((domain->num_exports >= domain->max_exports) ||
        (length > domain->max_export_bytes) ||
        (domain->export_bytes > (domain->max_export_bytes - length))) {
        status = UCS_ERR_NO_RESOURCE;
        goto out_unlock;
    }

    domain->export_bytes += length;
    ++domain->num_exports;
    status = UCS_OK;

out_unlock:
    ucs_spin_unlock(&domain->lock);
    return status;
}

static void
uct_obmm_domain_release_export(uct_obmm_resource_domain_t *domain, size_t length)
{
    ucs_spin_lock(&domain->lock);
    if (domain->export_bytes >= length) {
        domain->export_bytes -= length;
    } else {
        domain->export_bytes = 0;
    }
    if (domain->num_exports > 0) {
        --domain->num_exports;
    }
    ucs_spin_unlock(&domain->lock);
}

static ucs_status_t
uct_obmm_domain_reserve_import(uct_obmm_resource_domain_t *domain, size_t length)
{
    ucs_status_t status;

    ucs_spin_lock(&domain->lock);
    if ((domain->num_imports >= domain->max_imports) ||
        (length > domain->max_import_bytes) ||
        (domain->import_bytes > (domain->max_import_bytes - length))) {
        status = UCS_ERR_NO_RESOURCE;
        goto out_unlock;
    }

    domain->import_bytes += length;
    ++domain->num_imports;
    status = UCS_OK;

out_unlock:
    ucs_spin_unlock(&domain->lock);
    return status;
}

static void
uct_obmm_domain_release_import(uct_obmm_resource_domain_t *domain, size_t length)
{
    ucs_spin_lock(&domain->lock);
    if (domain->import_bytes >= length) {
        domain->import_bytes -= length;
    } else {
        domain->import_bytes = 0;
    }
    if (domain->num_imports > 0) {
        --domain->num_imports;
    }
    ucs_spin_unlock(&domain->lock);
}

static ucs_status_t
uct_obmm_domain_reserve_preimport(uct_obmm_resource_domain_t *domain, size_t length)
{
    ucs_status_t status;

    ucs_spin_lock(&domain->lock);
    if ((domain->num_preimports >= domain->max_preimports) ||
        (length > domain->max_preimport_bytes) ||
        (domain->preimport_bytes > (domain->max_preimport_bytes - length))) {
        status = UCS_ERR_NO_RESOURCE;
        goto out_unlock;
    }

    domain->preimport_bytes += length;
    ++domain->num_preimports;
    status = UCS_OK;

out_unlock:
    ucs_spin_unlock(&domain->lock);
    return status;
}

static void
uct_obmm_domain_release_preimport(uct_obmm_resource_domain_t *domain, size_t length)
{
    ucs_spin_lock(&domain->lock);
    if (domain->preimport_bytes >= length) {
        domain->preimport_bytes -= length;
    } else {
        domain->preimport_bytes = 0;
    }
    if (domain->num_preimports > 0) {
        --domain->num_preimports;
    }
    ucs_spin_unlock(&domain->lock);
}

ucs_status_t uct_obmm_errno_to_ucs_status(int err)
{
    switch (err) {
    case EINVAL:
        return UCS_ERR_INVALID_PARAM;
    case ENOMEM:
        return UCS_ERR_NO_MEMORY;
    case ENODEV:
        return UCS_ERR_NO_DEVICE;
    case EPERM:
        return UCS_ERR_UNSUPPORTED;
    case EBUSY:
    case EEXIST:
        return UCS_ERR_BUSY;
    case ENOSPC:
        return UCS_ERR_NO_RESOURCE;
    default:
        return UCS_ERR_IO_ERROR;
    }
}

ucs_status_t
uct_obmm_resource_domain_init(uct_obmm_resource_domain_t *domain,
                              const uct_obmm_md_runtime_config_t *config)
{
    ucs_status_t status;

    memset(domain, 0, sizeof(*domain));
    status = ucs_spinlock_init(&domain->lock, 0);
    if (status != UCS_OK) {
        return status;
    }

    domain->max_export_bytes   = config->max_export_bytes;
    domain->max_import_bytes   = config->max_import_bytes;
    domain->max_preimport_bytes = config->max_preimport_bytes;
    domain->max_exports        = config->max_exports;
    domain->max_imports        = config->max_imports;
    domain->max_preimports     = config->max_preimports;
    domain->exports            = kh_init(obmm_memh);
    domain->imports            = kh_init(obmm_imp);
    domain->preimports         = kh_init(obmm_pre);

    if ((domain->exports == NULL) || (domain->imports == NULL) ||
        (domain->preimports == NULL)) {
        uct_obmm_resource_domain_cleanup(domain);
        return UCS_ERR_NO_MEMORY;
    }

    return UCS_OK;
}

void uct_obmm_resource_domain_cleanup(uct_obmm_resource_domain_t *domain)
{
    khiter_t iter;
    uct_obmm_memh_t *memh;
    uct_obmm_import_res_t *import_res;
    uct_obmm_preimport_res_t *preimport_res;
    size_t preinfo_size;
    struct obmm_preimport_info *preinfo;

    if (domain == NULL) {
        return;
    }

    if (domain->exports != NULL) {
        for (iter = kh_begin(domain->exports); iter != kh_end(domain->exports);
             ++iter) {
            if (!kh_exist(domain->exports, iter)) {
                continue;
            }

            memh = kh_val(domain->exports, iter);
            if (uct_obmm_api_is_ready() && (memh != NULL) &&
                (memh->memid != OBMM_INVALID_MEMID)) {
                if (uct_obmm_api.obmm_unexport((mem_id)memh->memid, 0) != 0) {
                    ucs_warn("failed to unexport obmm memid %" PRIu64 ": %m",
                             memh->memid);
                }
            }
            ucs_free(memh);
        }
        kh_destroy(obmm_memh, domain->exports);
        domain->exports = NULL;
    }

    if (domain->imports != NULL) {
        for (iter = kh_begin(domain->imports); iter != kh_end(domain->imports);
             ++iter) {
            if (!kh_exist(domain->imports, iter)) {
                continue;
            }

            import_res = kh_val(domain->imports, iter);
            if ((import_res != NULL) &&
                (import_res->state == UCT_OBMM_IMPORT_MAPPED) &&
                (import_res->mapped_addr != NULL)) {
                munmap(import_res->mapped_addr, import_res->length);
            }

            if (uct_obmm_api_is_ready() && (import_res != NULL) &&
                (import_res->imported_memid != OBMM_INVALID_MEMID)) {
                if (uct_obmm_api.obmm_unimport((mem_id)import_res->imported_memid,
                                               0) != 0) {
                    ucs_warn("failed to unimport obmm memid %" PRIu64 ": %m",
                             import_res->imported_memid);
                }
            }
            ucs_free(import_res);
        }
        kh_destroy(obmm_imp, domain->imports);
        domain->imports = NULL;
    }

    if (domain->preimports != NULL) {
        for (iter = kh_begin(domain->preimports); iter != kh_end(domain->preimports);
             ++iter) {
            if (!kh_exist(domain->preimports, iter)) {
                continue;
            }

            preimport_res = kh_val(domain->preimports, iter);
            if (uct_obmm_api_is_ready() && (preimport_res != NULL)) {
                preinfo_size = sizeof(*preinfo) + preimport_res->priv_len;
                preinfo      = ucs_calloc(1, preinfo_size, "obmm_preinfo");
                if (preinfo != NULL) {
                    preinfo->pa       = preimport_res->pa;
                    preinfo->length   = preimport_res->length;
                    preinfo->base_dist = preimport_res->base_dist;
                    preinfo->numa_id  = preimport_res->numa_id;
                    preinfo->scna     = preimport_res->scna;
                    preinfo->dcna     = preimport_res->dcna;
                    preinfo->priv_len = preimport_res->priv_len;
                    memcpy(preinfo->seid, preimport_res->seid,
                           sizeof(preinfo->seid));
                    memcpy(preinfo->deid, preimport_res->deid,
                           sizeof(preinfo->deid));
                    memcpy(preinfo->priv, preimport_res->priv,
                           preimport_res->priv_len);
                    uct_obmm_api.obmm_unpreimport(preinfo, 0);
                    ucs_free(preinfo);
                }
            }
            ucs_free(preimport_res);
        }
        kh_destroy(obmm_pre, domain->preimports);
        domain->preimports = NULL;
    }

    ucs_spinlock_destroy(&domain->lock);
    memset(domain, 0, sizeof(*domain));
}

ucs_status_t uct_obmm_md_mem_reg(uct_md_h md, void *address, size_t length,
                                 const uct_md_mem_reg_params_t *params,
                                 uct_mem_h *memh_p)
{
    uct_obmm_md_t *obmm_md = ucs_derived_of(md, uct_obmm_md_t);
    uct_obmm_device_t *device;
    uct_obmm_memh_t *memh;
    struct obmm_mem_desc *desc;
    size_t desc_size;
    ucs_status_t status;
    mem_id memid;
    khiter_t iter;
    int kh_ret;

    (void)params;

    if ((address == NULL) || (length == 0) || (memh_p == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (!uct_obmm_is_aligned((uintptr_t)address, length,
                             obmm_md->topology.obmm_granularity)) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (!uct_obmm_api_is_ready()) {
        ucs_error("libobmm is not available");
        return UCS_ERR_UNSUPPORTED;
    }

    device = uct_obmm_md_select_device(obmm_md);
    if (!uct_obmm_is_device_usable(device)) {
        return UCS_ERR_NO_DEVICE;
    }

    status = uct_obmm_domain_reserve_export(device->res_domain, length);
    if (status != UCS_OK) {
        return status;
    }

    desc_size = sizeof(*desc);
    desc      = ucs_calloc(1, desc_size, "obmm_mem_desc");
    if (desc == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_release_quota;
    }

    desc->priv_len = 0;
    memcpy(desc->deid, device->primary_ctl->eid, sizeof(desc->deid));

    memid = uct_obmm_api.obmm_export_useraddr(0, address, length,
                                              OBMM_EXPORT_FLAG_ALLOW_MMAP, desc);
    if (memid == OBMM_INVALID_MEMID) {
        status = uct_obmm_errno_to_ucs_status(errno);
        ucs_error("failed to export obmm memory on %s: %m", device->name);
        goto out_free_desc;
    }

    memh = ucs_calloc(1, sizeof(*memh), "obmm_memh");
    if (memh == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_unexport;
    }

    memh->memid        = memid;
    memh->address      = address;
    memh->remote_addr  = desc->addr;
    memh->length       = length;
    memh->device_index = (int)device->index;
    memh->tokenid      = desc->tokenid;
    memh->scna         = desc->scna;
    memh->dcna         = desc->dcna;
    memh->priv_len     = ucs_min(desc->priv_len, (uint16_t)sizeof(memh->priv));
    memcpy(memh->seid, desc->seid, sizeof(memh->seid));
    memcpy(memh->deid, desc->deid, sizeof(memh->deid));
    memcpy(memh->priv, desc->priv, memh->priv_len);
    memh->export_flags = OBMM_EXPORT_FLAG_ALLOW_MMAP;
    memh->state        = UCT_OBMM_MEMH_EXPORTED;
    memh->device       = device;
    memh->refcount     = 0;

    ucs_spin_lock(&device->res_domain->lock);
    iter = kh_put(obmm_memh, device->res_domain->exports, memh->memid, &kh_ret);
    if (kh_ret < 0) {
        status = UCS_ERR_NO_MEMORY;
    } else if (kh_ret == 0) {
        status = UCS_ERR_BUSY;
    } else {
        kh_val(device->res_domain->exports, iter) = memh;
        status                                    = UCS_OK;
    }
    ucs_spin_unlock(&device->res_domain->lock);

    if (status != UCS_OK) {
        goto out_free_memh;
    }

    *memh_p = memh;
    ucs_free(desc);
    return UCS_OK;

out_free_memh:
    ucs_free(memh);
out_unexport:
    uct_obmm_api.obmm_unexport(memid, 0);
out_free_desc:
    ucs_free(desc);
out_release_quota:
    uct_obmm_domain_release_export(device->res_domain, length);
    return status;
}

ucs_status_t uct_obmm_md_mkey_pack(uct_md_h md, uct_mem_h memh, void *address,
                                   size_t length,
                                   const uct_md_mkey_pack_params_t *params,
                                   void *buffer)
{
    uct_obmm_memh_t *obmm_memh = memh;
    uct_obmm_packed_rkey_v1_t *packed = buffer;

    if ((obmm_memh == NULL) || (packed == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    (void)md;
    (void)address;
    (void)length;
    (void)params;

    if (obmm_memh->state < UCT_OBMM_MEMH_EXPORTED) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (obmm_memh->priv_len > UCT_OBMM_MAX_PRIV_LEN) {
        return UCS_ERR_INVALID_PARAM;
    }

    packed->version      = UCT_OBMM_RKEY_VERSION;
    packed->flags        = 0;
    packed->device_index = (uint32_t)obmm_memh->device_index;
    packed->memid        = obmm_memh->memid;
    packed->remote_addr  = obmm_memh->remote_addr;
    packed->length       = obmm_memh->length;
    packed->tokenid      = obmm_memh->tokenid;
    packed->scna         = obmm_memh->scna;
    packed->dcna         = obmm_memh->dcna;
    packed->priv_len     = obmm_memh->priv_len;
    packed->reserved     = 0;
    memcpy(packed->seid, obmm_memh->seid, sizeof(packed->seid));
    memcpy(packed->deid, obmm_memh->deid, sizeof(packed->deid));
    if (packed->priv_len > 0) {
        memcpy(packed->priv, obmm_memh->priv, packed->priv_len);
    }

    obmm_memh->state = UCT_OBMM_MEMH_RKEY_PACKED;
    return UCS_OK;
}

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p)
{
    const uct_obmm_packed_rkey_v1_t *packed = rkey_buffer;
    uct_obmm_import_res_t *import_res;
    uct_obmm_device_t *device;
    uct_obmm_resource_domain_t *domain;
    ucs_status_t status;
    khiter_t iter;
    int kh_ret;

    (void)component;

    if ((rkey_buffer == NULL) || (rkey_p == NULL) || (handle_p == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (packed->version != UCT_OBMM_RKEY_VERSION) {
        return UCS_ERR_UNSUPPORTED;
    }

    if (packed->priv_len > UCT_OBMM_MAX_PRIV_LEN) {
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_obmm_md_get_device_by_index(packed->device_index, &device);
    if (status != UCS_OK) {
        return status;
    }

    import_res = ucs_calloc(1, sizeof(*import_res), "obmm_import_res");
    if (import_res == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    import_res->memid         = packed->memid;
    import_res->imported_memid = OBMM_INVALID_MEMID;
    import_res->device_index  = packed->device_index;
    import_res->remote_addr   = packed->remote_addr;
    import_res->length        = packed->length;
    import_res->tokenid       = packed->tokenid;
    import_res->scna          = packed->scna;
    import_res->dcna          = packed->dcna;
    import_res->priv_len      = packed->priv_len;
    import_res->import_flags  = OBMM_IMPORT_FLAG_ALLOW_MMAP;
    import_res->base_dist     = 0;
    import_res->numa_id       = -1;
    import_res->state         = UCT_OBMM_IMPORT_INIT;
    import_res->refcount      = 1;
    import_res->device        = device;
    memcpy(import_res->seid, packed->seid, sizeof(import_res->seid));
    memcpy(import_res->deid, packed->deid, sizeof(import_res->deid));
    memcpy(import_res->priv, packed->priv, import_res->priv_len);

    domain = device->res_domain;
    ucs_spin_lock(&domain->lock);
    iter = kh_put(obmm_imp, domain->imports, import_res->memid, &kh_ret);
    if (kh_ret < 0) {
        status = UCS_ERR_NO_MEMORY;
    } else if (kh_ret == 0) {
        status = UCS_ERR_BUSY;
    } else {
        kh_val(domain->imports, iter) = import_res;
        status                        = UCS_OK;
    }
    ucs_spin_unlock(&domain->lock);

    if (status != UCS_OK) {
        ucs_free(import_res);
        return status;
    }

    *rkey_p   = 0;
    *handle_p = import_res;
    return UCS_OK;
}

static ucs_status_t uct_obmm_import_and_map(uct_obmm_import_res_t *import_res)
{
    uct_obmm_device_t *device = import_res->device;
    uct_obmm_resource_domain_t *domain = device->res_domain;
    struct obmm_mem_desc *desc;
    size_t desc_size;
    unsigned long import_flags;
    mem_id memid;
    char memdev_path[64];
    int fd, numa;
    void *mapped_addr;
    ucs_status_t status;

    if (!uct_obmm_api_is_ready()) {
        return UCS_ERR_UNSUPPORTED;
    }

    status = uct_obmm_domain_reserve_import(domain, import_res->length);
    if (status != UCS_OK) {
        return status;
    }
    import_res->quota_reserved = 1;

    desc_size = sizeof(*desc) + import_res->priv_len;
    desc      = ucs_calloc(1, desc_size, "obmm_import_desc");
    if (desc == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_release_quota;
    }

    desc->addr     = import_res->remote_addr;
    desc->length   = import_res->length;
    desc->tokenid  = import_res->tokenid;
    desc->scna     = device->primary_ctl->primary_cna;
    desc->dcna     = import_res->dcna;
    desc->priv_len = import_res->priv_len;
    memcpy(desc->seid, device->primary_ctl->eid, sizeof(desc->seid));
    memcpy(desc->deid, import_res->deid, sizeof(desc->deid));
    memcpy(desc->priv, import_res->priv, import_res->priv_len);

    import_flags = OBMM_IMPORT_FLAG_ALLOW_MMAP;
    if (import_res->state == UCT_OBMM_IMPORT_PREIMPORTED) {
        import_flags = OBMM_IMPORT_FLAG_NUMA_REMOTE | OBMM_IMPORT_FLAG_PREIMPORT;
    }

    numa  = import_res->numa_id;
    memid = uct_obmm_api.obmm_import(desc, import_flags, import_res->base_dist,
                                     &numa);
    if (memid == OBMM_INVALID_MEMID) {
        status = uct_obmm_errno_to_ucs_status(errno);
        goto out_free_desc;
    }

    ucs_snprintf_safe(memdev_path, sizeof(memdev_path), "/dev/obmm_shmdev%" PRIu64,
                      (uint64_t)memid);
    fd = open(memdev_path, O_RDWR);
    if (fd < 0) {
        status = uct_obmm_errno_to_ucs_status(errno);
        goto out_unimport;
    }

    mapped_addr = mmap(NULL, import_res->length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    close(fd);
    if (mapped_addr == MAP_FAILED) {
        status = UCS_ERR_IO_ERROR;
        goto out_unimport;
    }

    import_res->imported_memid = memid;
    import_res->mapped_addr    = mapped_addr;
    import_res->numa_id        = numa;
    import_res->import_flags   = import_flags;
    import_res->state          = UCT_OBMM_IMPORT_MAPPED;
    ucs_free(desc);
    return UCS_OK;

out_unimport:
    uct_obmm_api.obmm_unimport(memid, 0);
out_free_desc:
    ucs_free(desc);
out_release_quota:
    if (import_res->quota_reserved) {
        import_res->quota_reserved = 0;
        uct_obmm_domain_release_import(domain, import_res->length);
    }
    return status;
}

ucs_status_t uct_obmm_rkey_ptr(uct_component_t *component, uct_rkey_t rkey,
                               void *handle, uint64_t remote_addr,
                               void **local_addr_p)
{
    uct_obmm_import_res_t *import_res = handle;
    uintptr_t offset;
    ucs_status_t status;

    (void)component;
    (void)rkey;

    if ((import_res == NULL) || (local_addr_p == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    if ((import_res->state == UCT_OBMM_IMPORT_INIT) ||
        (import_res->state == UCT_OBMM_IMPORT_PREIMPORTED)) {
        status = uct_obmm_import_and_map(import_res);
        if (status != UCS_OK) {
            return status;
        }
    }

    if (remote_addr < import_res->remote_addr) {
        return UCS_ERR_INVALID_ADDR;
    }

    offset = remote_addr - import_res->remote_addr;
    if (offset >= import_res->length) {
        return UCS_ERR_INVALID_ADDR;
    }

    *local_addr_p = UCS_PTR_BYTE_OFFSET(import_res->mapped_addr, offset);
    return UCS_OK;
}

ucs_status_t uct_obmm_rkey_release(uct_component_t *component, uct_rkey_t rkey,
                                   void *handle)
{
    uct_obmm_import_res_t *import_res = handle;
    uct_obmm_preimport_res_t *preimport_res;
    uct_obmm_resource_domain_t *domain;
    struct obmm_preimport_info *preinfo;
    size_t preinfo_size;
    khiter_t iter;

    (void)component;
    (void)rkey;

    if (import_res == NULL) {
        return UCS_OK;
    }

    if (import_res->refcount > 1) {
        --import_res->refcount;
        return UCS_OK;
    }

    domain = import_res->device->res_domain;

    if ((import_res->state == UCT_OBMM_IMPORT_MAPPED) &&
        (import_res->mapped_addr != NULL)) {
        if (munmap(import_res->mapped_addr, import_res->length) != 0) {
            ucs_warn("failed to munmap obmm memory %" PRIu64 ": %m",
                     import_res->imported_memid);
        }

        if (uct_obmm_api_is_ready() &&
            (import_res->imported_memid != OBMM_INVALID_MEMID) &&
            (uct_obmm_api.obmm_unimport((mem_id)import_res->imported_memid, 0) != 0)) {
            ucs_warn("failed to unimport obmm memory %" PRIu64 ": %m",
                     import_res->imported_memid);
        }
    }

    ucs_spin_lock(&domain->lock);
    iter = kh_get(obmm_imp, domain->imports, import_res->memid);
    if ((iter != kh_end(domain->imports)) &&
        (kh_val(domain->imports, iter) == import_res)) {
        kh_del(obmm_imp, domain->imports, iter);
    }
    ucs_spin_unlock(&domain->lock);

    if (import_res->quota_reserved) {
        uct_obmm_domain_release_import(domain, import_res->length);
    }

    preimport_res = import_res->preimport;
    if (preimport_res != NULL) {
        ucs_spin_lock(&domain->lock);
        iter = kh_get(obmm_pre, domain->preimports, preimport_res->key);
        if ((iter != kh_end(domain->preimports)) &&
            (kh_val(domain->preimports, iter) == preimport_res)) {
            kh_del(obmm_pre, domain->preimports, iter);
        }
        ucs_spin_unlock(&domain->lock);

        if (uct_obmm_api_is_ready()) {
            preinfo_size = sizeof(*preinfo) + preimport_res->priv_len;
            preinfo      = ucs_calloc(1, preinfo_size, "obmm_preimport_info");
            if (preinfo != NULL) {
                preinfo->pa        = preimport_res->pa;
                preinfo->length    = preimport_res->length;
                preinfo->base_dist = preimport_res->base_dist;
                preinfo->numa_id   = preimport_res->numa_id;
                preinfo->scna      = preimport_res->scna;
                preinfo->dcna      = preimport_res->dcna;
                preinfo->priv_len  = preimport_res->priv_len;
                memcpy(preinfo->seid, preimport_res->seid, sizeof(preinfo->seid));
                memcpy(preinfo->deid, preimport_res->deid, sizeof(preinfo->deid));
                memcpy(preinfo->priv, preimport_res->priv, preimport_res->priv_len);
                uct_obmm_api.obmm_unpreimport(preinfo, 0);
                ucs_free(preinfo);
            }
        }

        uct_obmm_domain_release_preimport(domain, preimport_res->length);
        ucs_free(preimport_res);
    }

    import_res->state = UCT_OBMM_IMPORT_RELEASED;
    ucs_free(import_res);
    return UCS_OK;
}

ucs_status_t uct_obmm_md_mem_dereg(uct_md_h md,
                                   const uct_md_mem_dereg_params_t *params)
{
    uct_obmm_memh_t *memh;
    uct_obmm_resource_domain_t *domain;
    khiter_t iter;

    (void)md;

    UCT_MD_MEM_DEREG_CHECK_PARAMS(params, 0);

    memh = (uct_obmm_memh_t*)params->memh;
    if (memh->refcount != 0) {
        return UCS_ERR_BUSY;
    }

    domain = memh->device->res_domain;
    memh->state = UCT_OBMM_MEMH_RELEASE_PENDING;

    if (uct_obmm_api_is_ready() &&
        (uct_obmm_api.obmm_unexport((mem_id)memh->memid, 0) != 0)) {
        ucs_error("failed to unexport obmm memory %" PRIu64 ": %m", memh->memid);
        return uct_obmm_errno_to_ucs_status(errno);
    }

    ucs_spin_lock(&domain->lock);
    iter = kh_get(obmm_memh, domain->exports, memh->memid);
    if ((iter != kh_end(domain->exports)) &&
        (kh_val(domain->exports, iter) == memh)) {
        kh_del(obmm_memh, domain->exports, iter);
    }
    ucs_spin_unlock(&domain->lock);

    uct_obmm_domain_release_export(domain, memh->length);
    memh->state = UCT_OBMM_MEMH_RELEASED;
    ucs_free(memh);
    return UCS_OK;
}

ucs_status_t uct_obmm_preimport_reserve(uct_obmm_device_t *device,
                                        const uct_obmm_packed_rkey_v1_t *rkey,
                                        int base_dist,
                                        uct_obmm_import_res_t **res_p)
{
    uct_obmm_preimport_res_t *preimport_res;
    uct_obmm_import_res_t *import_res;
    uct_obmm_resource_domain_t *domain;
    struct obmm_preimport_info *preinfo;
    size_t preinfo_size;
    uint64_t key;
    khiter_t iter;
    int kh_ret;
    ucs_status_t status;

    if ((device == NULL) || (rkey == NULL) || (res_p == NULL) ||
        !uct_obmm_is_device_usable(device)) {
        return UCS_ERR_INVALID_PARAM;
    }

    if (!uct_obmm_api_is_ready()) {
        return UCS_ERR_UNSUPPORTED;
    }

    domain = device->res_domain;
    status = uct_obmm_domain_reserve_preimport(domain, rkey->length);
    if (status != UCS_OK) {
        return status;
    }

    key = uct_obmm_preimport_key(rkey->remote_addr, rkey->length);
    ucs_spin_lock(&domain->lock);
    iter = kh_get(obmm_pre, domain->preimports, key);
    if (iter != kh_end(domain->preimports)) {
        ucs_spin_unlock(&domain->lock);
        uct_obmm_domain_release_preimport(domain, rkey->length);
        return UCS_ERR_ALREADY_EXISTS;
    }
    ucs_spin_unlock(&domain->lock);

    preimport_res = ucs_calloc(1, sizeof(*preimport_res), "obmm_preimport_res");
    if (preimport_res == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_release_quota;
    }

    preimport_res->key       = key;
    preimport_res->pa        = rkey->remote_addr;
    preimport_res->length    = rkey->length;
    preimport_res->base_dist = base_dist;
    preimport_res->numa_id   = -1;
    preimport_res->scna      = device->primary_ctl->primary_cna;
    preimport_res->dcna      = rkey->dcna;
    preimport_res->priv_len  = ucs_min(rkey->priv_len,
                                       (uint16_t)UCT_OBMM_MAX_PRIV_LEN);
    preimport_res->refcount  = 1;
    preimport_res->device    = device;
    memcpy(preimport_res->seid, device->primary_ctl->eid, sizeof(preimport_res->seid));
    memcpy(preimport_res->deid, rkey->deid, sizeof(preimport_res->deid));
    memcpy(preimport_res->priv, rkey->priv, preimport_res->priv_len);

    preinfo_size = sizeof(*preinfo) + preimport_res->priv_len;
    preinfo      = ucs_calloc(1, preinfo_size, "obmm_preimport_info");
    if (preinfo == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_free_preimport;
    }

    preinfo->pa        = preimport_res->pa;
    preinfo->length    = preimport_res->length;
    preinfo->base_dist = preimport_res->base_dist;
    preinfo->numa_id   = preimport_res->numa_id;
    preinfo->scna      = preimport_res->scna;
    preinfo->dcna      = preimport_res->dcna;
    preinfo->priv_len  = preimport_res->priv_len;
    memcpy(preinfo->seid, preimport_res->seid, sizeof(preinfo->seid));
    memcpy(preinfo->deid, preimport_res->deid, sizeof(preinfo->deid));
    memcpy(preinfo->priv, preimport_res->priv, preimport_res->priv_len);

    if (uct_obmm_api.obmm_preimport(preinfo, 0) != 0) {
        status = uct_obmm_errno_to_ucs_status(errno);
        ucs_free(preinfo);
        goto out_free_preimport;
    }

    preimport_res->numa_id = preinfo->numa_id;
    ucs_free(preinfo);

    import_res = ucs_calloc(1, sizeof(*import_res), "obmm_preimport_import");
    if (import_res == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto out_rollback_preimport;
    }

    import_res->memid          = rkey->memid;
    import_res->imported_memid = OBMM_INVALID_MEMID;
    import_res->device_index   = device->index;
    import_res->numa_id        = preimport_res->numa_id;
    import_res->base_dist      = base_dist;
    import_res->remote_addr    = rkey->remote_addr;
    import_res->length         = rkey->length;
    import_res->tokenid        = rkey->tokenid;
    import_res->scna           = preimport_res->scna;
    import_res->dcna           = preimport_res->dcna;
    import_res->state          = UCT_OBMM_IMPORT_PREIMPORTED;
    import_res->refcount       = 1;
    import_res->device         = device;
    import_res->preimport      = preimport_res;
    import_res->priv_len       = preimport_res->priv_len;
    memcpy(import_res->seid, preimport_res->seid, sizeof(import_res->seid));
    memcpy(import_res->deid, preimport_res->deid, sizeof(import_res->deid));
    memcpy(import_res->priv, preimport_res->priv, import_res->priv_len);

    ucs_spin_lock(&domain->lock);
    iter = kh_put(obmm_pre, domain->preimports, key, &kh_ret);
    if (kh_ret < 0) {
        status = UCS_ERR_NO_MEMORY;
    } else if (kh_ret == 0) {
        status = UCS_ERR_ALREADY_EXISTS;
    } else {
        kh_val(domain->preimports, iter) = preimport_res;
        status                           = UCS_OK;
    }
    ucs_spin_unlock(&domain->lock);

    if (status != UCS_OK) {
        goto out_free_import;
    }

    *res_p = import_res;
    return UCS_OK;

out_free_import:
    ucs_free(import_res);
out_rollback_preimport:
    preinfo_size = sizeof(*preinfo) + preimport_res->priv_len;
    preinfo      = ucs_calloc(1, preinfo_size, "obmm_preimport_info");
    if (preinfo != NULL) {
        preinfo->pa        = preimport_res->pa;
        preinfo->length    = preimport_res->length;
        preinfo->base_dist = preimport_res->base_dist;
        preinfo->numa_id   = preimport_res->numa_id;
        preinfo->scna      = preimport_res->scna;
        preinfo->dcna      = preimport_res->dcna;
        preinfo->priv_len  = preimport_res->priv_len;
        memcpy(preinfo->seid, preimport_res->seid, sizeof(preinfo->seid));
        memcpy(preinfo->deid, preimport_res->deid, sizeof(preinfo->deid));
        memcpy(preinfo->priv, preimport_res->priv, preimport_res->priv_len);
        uct_obmm_api.obmm_unpreimport(preinfo, 0);
        ucs_free(preinfo);
    }
out_free_preimport:
    ucs_free(preimport_res);
out_release_quota:
    uct_obmm_domain_release_preimport(domain, rkey->length);
    return status;
}
