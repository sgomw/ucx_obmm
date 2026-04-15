/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_MD_H_
#define UCT_OBMM_MD_H_

#include <uct/base/uct_md.h>
#include <ucs/datastruct/khash.h>
#include <ucs/sys/topo/base/topo.h>
#include <ucs/type/cpu_set.h>
#include <ucs/type/spinlock.h>
#include <stdint.h>
#include <limits.h>


#define UCT_OBMM_IFACE_ADDR_VERSION         1
#define UCT_OBMM_RKEY_VERSION               1
#define UCT_OBMM_DEFAULT_CTL_GLOB           "/sys/devices/ub_bus_controller*/*/ubc"
#define UCT_OBMM_DEFAULT_GRANULARITY        (2ul * 1024 * 1024)
#define UCT_OBMM_DEFAULT_MAX_EXPORT_BYTES   SIZE_MAX
#define UCT_OBMM_DEFAULT_MAX_IMPORT_BYTES   SIZE_MAX
#define UCT_OBMM_DEFAULT_MAX_PREIMPORT_BYTES SIZE_MAX
#define UCT_OBMM_DEFAULT_MAX_EXPORTS        4096
#define UCT_OBMM_DEFAULT_MAX_IMPORTS        4096
#define UCT_OBMM_DEFAULT_MAX_PREIMPORTS     4096
#define UCT_OBMM_DEFAULT_SOCKET_AUTO        (-1)
#define UCT_OBMM_DEFAULT_ENABLE_PREIMPORT   1
#define UCT_OBMM_DEFAULT_BW                 (12179.0 * 1024 * 1024)
#define UCT_OBMM_DEFAULT_LATENCY            80e-9
#define UCT_OBMM_DEFAULT_OVERHEAD           100e-9
#define UCT_OBMM_DEFAULT_PRIORITY           0
#define UCT_OBMM_MAX_PRIV_LEN               512


struct uct_obmm_memh;
struct uct_obmm_import_res;
struct uct_obmm_preimport_res;
struct uct_obmm_resource_domain;
typedef struct uct_obmm_device uct_obmm_device_t;

KHASH_MAP_INIT_INT64(obmm_memh, struct uct_obmm_memh*);
KHASH_MAP_INIT_INT64(obmm_imp, struct uct_obmm_import_res*);
KHASH_MAP_INIT_INT64(obmm_pre, struct uct_obmm_preimport_res*);

enum {
    UCT_OBMM_DEVICE_FLAG_DEGRADED = UCS_BIT(0)
};

typedef enum uct_obmm_memh_state {
    UCT_OBMM_MEMH_INIT,
    UCT_OBMM_MEMH_EXPORTED,
    UCT_OBMM_MEMH_RKEY_PACKED,
    UCT_OBMM_MEMH_RELEASE_PENDING,
    UCT_OBMM_MEMH_RELEASED
} uct_obmm_memh_state_t;

typedef enum uct_obmm_import_state {
    UCT_OBMM_IMPORT_INIT,
    UCT_OBMM_IMPORT_PREIMPORTED,
    UCT_OBMM_IMPORT_IMPORTED,
    UCT_OBMM_IMPORT_MAPPED,
    UCT_OBMM_IMPORT_RELEASE_PENDING,
    UCT_OBMM_IMPORT_RELEASED
} uct_obmm_import_state_t;

typedef struct uct_obmm_controller {
    uint8_t  eid[16];
    uint32_t primary_cna;
    int      numa_id;
    int      socket_id;
    int      ummu_mapping;
    char     sysfs_path[PATH_MAX];
    uint64_t flags;
} uct_obmm_controller_t;

typedef struct uct_obmm_device_caps {
    uct_device_type_t type;
    ucs_sys_device_t  sys_device;
    double            dedicated_bw;
    double            shared_bw;
    ucs_linear_func_t latency;
    double            overhead;
    size_t            reg_alignment;
    size_t            obmm_granularity;
    uint64_t          iface_flags;
    uint64_t          md_flags;
} uct_obmm_device_caps_t;

typedef struct uct_obmm_resource_domain {
    ucs_spinlock_t      lock;
    size_t              export_bytes;
    size_t              import_bytes;
    size_t              preimport_bytes;
    size_t              max_export_bytes;
    size_t              max_import_bytes;
    size_t              max_preimport_bytes;
    unsigned            max_exports;
    unsigned            max_imports;
    unsigned            max_preimports;
    unsigned            num_exports;
    unsigned            num_imports;
    unsigned            num_preimports;
    khash_t(obmm_memh) *exports;
    khash_t(obmm_imp)  *imports;
    khash_t(obmm_pre)  *preimports;
} uct_obmm_resource_domain_t;

struct uct_obmm_device {
    char                        name[UCT_DEVICE_NAME_MAX];
    uint32_t                    index;
    int                         socket_id;
    int                         numa_id;
    ucs_cpu_set_t               local_cpus;
    uct_obmm_controller_t      *primary_ctl;
    uct_obmm_controller_t      *controllers;
    unsigned                    num_controllers;
    uct_obmm_device_caps_t      caps;
    uct_obmm_resource_domain_t *res_domain;
    ucs_spinlock_t              lock;
    uint64_t                    generation;
    uint64_t                    flags;
};

typedef struct uct_obmm_topology {
    uct_obmm_device_t     *devices;
    unsigned               num_devices;
    uct_obmm_controller_t *controllers;
    unsigned               num_controllers;
    size_t                 obmm_granularity;
    uint64_t               generation;
    uint64_t               flags;
} uct_obmm_topology_t;

typedef struct uct_obmm_packed_rkey_v1 {
    uint16_t version;
    uint16_t flags;
    uint32_t device_index;
    uint64_t memid;
    uint64_t remote_addr;
    uint64_t length;
    uint32_t tokenid;
    uint32_t scna;
    uint32_t dcna;
    uint8_t  seid[16];
    uint8_t  deid[16];
    uint16_t priv_len;
    uint16_t reserved;
    uint8_t  priv[];
} UCS_S_PACKED uct_obmm_packed_rkey_v1_t;

typedef struct uct_obmm_memh {
    uint64_t                memid;
    void                   *address;
    uint64_t                remote_addr;
    size_t                  length;
    int                     device_index;
    uint32_t                tokenid;
    uint8_t                 seid[16];
    uint8_t                 deid[16];
    uint32_t                scna;
    uint32_t                dcna;
    uint16_t                priv_len;
    uint8_t                 priv[UCT_OBMM_MAX_PRIV_LEN];
    unsigned long           export_flags;
    uct_obmm_memh_state_t   state;
    uint32_t                refcount;
    uct_obmm_device_t      *device;
} uct_obmm_memh_t;

typedef struct uct_obmm_preimport_res {
    uint64_t      key;
    uint64_t      pa;
    size_t        length;
    int           numa_id;
    int           base_dist;
    uint32_t      scna;
    uint32_t      dcna;
    uint8_t       seid[16];
    uint8_t       deid[16];
    uint16_t      priv_len;
    uint8_t       priv[UCT_OBMM_MAX_PRIV_LEN];
    uint32_t      refcount;
    uct_obmm_device_t *device;
} uct_obmm_preimport_res_t;

typedef struct uct_obmm_import_res {
    uint64_t                  memid;
    uint64_t                  imported_memid;
    int                       device_index;
    int                       numa_id;
    unsigned long             import_flags;
    unsigned long             base_dist;
    uint64_t                  remote_addr;
    size_t                    length;
    void                     *mapped_addr;
    uct_obmm_import_state_t   state;
    uint32_t                  refcount;
    uint32_t                  tokenid;
    uint32_t                  scna;
    uint32_t                  dcna;
    uint8_t                   seid[16];
    uint8_t                   deid[16];
    uint16_t                  priv_len;
    uint8_t                   priv[UCT_OBMM_MAX_PRIV_LEN];
    int                       quota_reserved;
    uct_obmm_device_t        *device;
    uct_obmm_preimport_res_t *preimport;
} uct_obmm_import_res_t;

typedef struct uct_obmm_md_runtime_config {
    char     ctl_glob[PATH_MAX];
    size_t   granularity;
    size_t   max_export_bytes;
    size_t   max_import_bytes;
    size_t   max_preimport_bytes;
    unsigned max_exports;
    unsigned max_imports;
    unsigned max_preimports;
    int      default_socket;
    int      enable_preimport;
} uct_obmm_md_runtime_config_t;


typedef struct uct_obmm_md_config {
    uct_md_config_t super;
    char           *ctl_glob;
    size_t          granularity;
    size_t          max_export_bytes;
    size_t          max_import_bytes;
    size_t          max_preimport_bytes;
    unsigned        max_exports;
    unsigned        max_imports;
    unsigned        max_preimports;
    char           *default_socket;
    int             enable_preimport;
} uct_obmm_md_config_t;

typedef struct uct_obmm_md {
    uct_md_t                       super;
    uct_obmm_topology_t            topology;
    ucs_spinlock_t                 lock;
    ucs_cpu_set_t                  union_local_cpus;
    uint32_t                       generation;
    uct_obmm_md_runtime_config_t   config;
} uct_obmm_md_t;

extern ucs_config_field_t uct_obmm_md_config_table[];
extern uct_component_t uct_obmm_component;

ucs_status_t uct_obmm_topology_discover(uct_obmm_md_t *md,
                                        uct_obmm_topology_t *topology);

void uct_obmm_topology_cleanup(uct_obmm_topology_t *topology);

ucs_status_t uct_obmm_device_catalog_lookup(uct_obmm_md_t *md,
                                            const char *dev_name,
                                            uct_obmm_device_t **device_p);

ucs_status_t uct_obmm_resource_domain_init(uct_obmm_resource_domain_t *domain,
                                           const uct_obmm_md_runtime_config_t *config);

void uct_obmm_resource_domain_cleanup(uct_obmm_resource_domain_t *domain);

ucs_status_t uct_obmm_errno_to_ucs_status(int err);

ucs_status_t uct_obmm_md_mem_reg(uct_md_h md, void *address, size_t length,
                                 const uct_md_mem_reg_params_t *params,
                                 uct_mem_h *memh_p);

ucs_status_t uct_obmm_md_mem_dereg(uct_md_h md,
                                   const uct_md_mem_dereg_params_t *params);

ucs_status_t uct_obmm_md_mkey_pack(uct_md_h md, uct_mem_h memh, void *address,
                                   size_t length,
                                   const uct_md_mkey_pack_params_t *params,
                                   void *buffer);

ucs_status_t uct_obmm_md_open(uct_component_t *component, const char *md_name,
                              const uct_md_config_t *config, uct_md_h *md_p);

ucs_status_t uct_obmm_md_rkey_unpack(uct_component_t *component,
                                     const void *rkey_buffer,
                                     uct_rkey_t *rkey_p, void **handle_p);

ucs_status_t uct_obmm_rkey_ptr(uct_component_t *component, uct_rkey_t rkey,
                               void *handle, uint64_t remote_addr,
                               void **local_addr_p);

ucs_status_t uct_obmm_rkey_release(uct_component_t *component, uct_rkey_t rkey,
                                   void *handle);

ucs_status_t uct_obmm_md_get_device_by_index(uint32_t device_index,
                                             uct_obmm_device_t **device_p);

ucs_status_t uct_obmm_preimport_reserve(
        uct_obmm_device_t *device, const uct_obmm_packed_rkey_v1_t *rkey,
        int base_dist, uct_obmm_import_res_t **res_p);

#endif
