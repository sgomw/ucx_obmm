#include <uct/api/uct.h>
#include <uct/api/v2/uct_v2.h>
#include <ucs/async/async.h>
#include <ucs/type/status.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(_expr) \
    do { \
        status = (_expr); \
        if (status != UCS_OK) { \
            fprintf(stderr, "FAIL: %s -> %s\n", #_expr, ucs_status_string(status)); \
            goto out; \
        } \
    } while (0)

typedef enum {
    MODE_NORMAL,
    MODE_QUOTA_LIMIT,
    MODE_EXPECT_NO_TL
} run_mode_t;

static run_mode_t parse_mode(int argc, char **argv)
{
    if (argc < 2) {
        return MODE_NORMAL;
    }
    if (!strcmp(argv[1], "--quota-limit")) {
        return MODE_QUOTA_LIMIT;
    }
    if (!strcmp(argv[1], "--expect-no-tl")) {
        return MODE_EXPECT_NO_TL;
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    exit(2);
}

int main(int argc, char **argv)
{
    run_mode_t mode = parse_mode(argc, argv);
    ucs_status_t status = UCS_OK, tmp_status;
    uct_component_h *components = NULL, obmm_component = NULL;
    unsigned num_components = 0, i, num_obmm_tl = 0;
    uct_component_attr_t comp_attr;
    uct_md_resource_desc_t *md_resources = NULL;
    uct_md_config_t *md_config = NULL;
    uct_md_h md = NULL;
    uct_md_attr_v2_t md_attr;
    uct_tl_resource_desc_t *tl_resources = NULL;
    unsigned num_tl_resources = 0;
    ucs_async_context_t *async = NULL;
    uct_worker_h worker = NULL;
    uct_iface_config_t *iface_config = NULL;
    uct_iface_h iface = NULL;

    void *buf1 = NULL, *buf2 = NULL, *rkey_buf = NULL;
    uct_mem_h memh1 = UCT_MEM_HANDLE_NULL, memh2 = UCT_MEM_HANDLE_NULL;
    uct_rkey_bundle_t rkey_ob;
    int rkey_unpacked = 0;
    size_t align, length;
    uct_md_mem_reg_params_t reg_params;
    uct_md_mem_dereg_params_t dereg_params;
    uct_md_mkey_pack_params_t pack_params;
    uint64_t remote_addr;
    void *local0 = NULL, *local1 = NULL, *tmp = NULL;

    CHECK(uct_query_components(&components, &num_components));

    for (i = 0; i < num_components; ++i) {
        memset(&comp_attr, 0, sizeof(comp_attr));
        comp_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_NAME;
        CHECK(uct_component_query(components[i], &comp_attr));
        if (!strcmp(comp_attr.name, "obmm")) {
            obmm_component = components[i];
            break;
        }
    }

    if (obmm_component == NULL) {
        fprintf(stderr, "FAIL: obmm component not found\n");
        status = UCS_ERR_NO_DEVICE;
        goto out;
    }

    memset(&comp_attr, 0, sizeof(comp_attr));
    comp_attr.field_mask = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCE_COUNT;
    CHECK(uct_component_query(obmm_component, &comp_attr));
    if (comp_attr.md_resource_count == 0) {
        fprintf(stderr, "FAIL: obmm md_resource_count=0\n");
        status = UCS_ERR_NO_DEVICE;
        goto out;
    }

    md_resources = calloc(comp_attr.md_resource_count, sizeof(*md_resources));
    if (md_resources == NULL) {
        fprintf(stderr, "FAIL: no memory for md_resources\n");
        status = UCS_ERR_NO_MEMORY;
        goto out;
    }

    memset(&comp_attr, 0, sizeof(comp_attr));
    comp_attr.field_mask   = UCT_COMPONENT_ATTR_FIELD_MD_RESOURCES;
    comp_attr.md_resources = md_resources;
    CHECK(uct_component_query(obmm_component, &comp_attr));

    CHECK(uct_md_config_read(obmm_component, NULL, NULL, &md_config));
    CHECK(uct_md_open(obmm_component, md_resources[0].md_name, md_config, &md));

    memset(&md_attr, 0, sizeof(md_attr));
    md_attr.field_mask = UINT64_MAX;
    CHECK(uct_md_query_v2(md, &md_attr));

    CHECK(uct_md_query_tl_resources(md, &tl_resources, &num_tl_resources));

    for (i = 0; i < num_tl_resources; ++i) {
        if (!strcmp(tl_resources[i].tl_name, "obmm")) {
            ++num_obmm_tl;
        }
    }

    if (mode == MODE_EXPECT_NO_TL) {
        if (num_obmm_tl != 0) {
            fprintf(stderr, "FAIL: expected 0 obmm TL devices, got %u\n", num_obmm_tl);
            status = UCS_ERR_ALREADY_EXISTS;
            goto out;
        }
        printf("PASS: no obmm TL device as expected\n");
        status = UCS_OK;
        goto out;
    }

    if (num_obmm_tl == 0) {
        fprintf(stderr, "FAIL: no obmm TL device found\n");
        status = UCS_ERR_NO_DEVICE;
        goto out;
    }

    CHECK(ucs_async_context_create(UCS_ASYNC_MODE_THREAD_SPINLOCK, &async));
    CHECK(uct_worker_create(async, UCS_THREAD_MODE_SINGLE, &worker));

    for (i = 0; i < num_tl_resources; ++i) {
        uct_iface_params_t iface_params;

        if (strcmp(tl_resources[i].tl_name, "obmm")) {
            continue;
        }

        CHECK(uct_md_iface_config_read(md, tl_resources[i].tl_name, NULL, NULL, &iface_config));

        memset(&iface_params, 0, sizeof(iface_params));
        iface_params.field_mask           = UCT_IFACE_PARAM_FIELD_OPEN_MODE |
                                            UCT_IFACE_PARAM_FIELD_DEVICE;
        iface_params.open_mode            = UCT_IFACE_OPEN_MODE_DEVICE;
        iface_params.mode.device.tl_name  = tl_resources[i].tl_name;
        iface_params.mode.device.dev_name = tl_resources[i].dev_name;

        CHECK(uct_iface_open(md, worker, &iface_params, iface_config, &iface));
        printf("PASS: iface open on %s/%s\n", tl_resources[i].tl_name, tl_resources[i].dev_name);

        uct_iface_close(iface);
        iface = NULL;
        uct_config_release(iface_config);
        iface_config = NULL;
    }

    align = (md_attr.reg_alignment != 0) ? md_attr.reg_alignment : 4096;
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    length = (align < 4096) ? 4096 : align;

    if (posix_memalign(&buf1, align, length) != 0) {
        fprintf(stderr, "FAIL: posix_memalign buf1\n");
        status = UCS_ERR_NO_MEMORY;
        goto out;
    }
    memset(buf1, 0x5A, length);

    memset(&reg_params, 0, sizeof(reg_params));
    reg_params.field_mask = UCT_MD_MEM_REG_FIELD_FLAGS;
    reg_params.flags      = UCT_MD_MEM_ACCESS_REMOTE_RMA;

    CHECK(uct_md_mem_reg_v2(md, buf1, length, &reg_params, &memh1));

    if (mode == MODE_QUOTA_LIMIT) {
        if (posix_memalign(&buf2, align, length) != 0) {
            fprintf(stderr, "FAIL: posix_memalign buf2\n");
            status = UCS_ERR_NO_MEMORY;
            goto out;
        }
        memset(buf2, 0xA5, length);

        status = uct_md_mem_reg_v2(md, buf2, length, &reg_params, &memh2);
        if (status != UCS_ERR_EXCEEDS_LIMIT) {
            fprintf(stderr, "FAIL: second mem_reg expected UCS_ERR_EXCEEDS_LIMIT, got %s\n",
                    ucs_status_string(status));
            goto out;
        }

        printf("PASS: quota limit check (%s)\n", ucs_status_string(status));
        status = UCS_OK;
        goto out;
    }

    rkey_buf = calloc(1, md_attr.rkey_packed_size);
    if (rkey_buf == NULL) {
        fprintf(stderr, "FAIL: no memory for rkey buffer\n");
        status = UCS_ERR_NO_MEMORY;
        goto out;
    }

    memset(&pack_params, 0, sizeof(pack_params));
    pack_params.field_mask = UCT_MD_MKEY_PACK_FIELD_FLAGS;
    pack_params.flags      = 0;
    CHECK(uct_md_mkey_pack_v2(md, memh1, buf1, length, &pack_params, rkey_buf));

    CHECK(uct_rkey_unpack(obmm_component, rkey_buf, &rkey_ob));
    rkey_unpacked = 1;

    remote_addr = (uint64_t)(uintptr_t)buf1;
    CHECK(uct_rkey_ptr(obmm_component, &rkey_ob, remote_addr, &local0));
    CHECK(uct_rkey_ptr(obmm_component, &rkey_ob, remote_addr + 64, &local1));

    if (((char*)local1 - (char*)local0) != 64) {
        fprintf(stderr, "FAIL: rkey_ptr offset mismatch\n");
        status = UCS_ERR_INVALID_ADDR;
        goto out;
    }

    status = uct_rkey_ptr(obmm_component, &rkey_ob, remote_addr + length, &tmp);
    if (status != UCS_ERR_INVALID_ADDR) {
        fprintf(stderr, "FAIL: out-of-range rkey_ptr expected UCS_ERR_INVALID_ADDR, got %s\n",
                ucs_status_string(status));
        goto out;
    }

    printf("PASS: rkey lifecycle chain\n");
    status = UCS_OK;

out:
    if (rkey_unpacked) {
        tmp_status = uct_rkey_release(obmm_component, &rkey_ob);
        if ((status == UCS_OK) && (tmp_status != UCS_OK)) {
            status = tmp_status;
        }
    }

    if (memh2 != UCT_MEM_HANDLE_NULL) {
        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh2;
        tmp_status = uct_md_mem_dereg_v2(md, &dereg_params);
        if ((status == UCS_OK) && (tmp_status != UCS_OK)) {
            status = tmp_status;
        }
    }

    if (memh1 != UCT_MEM_HANDLE_NULL) {
        memset(&dereg_params, 0, sizeof(dereg_params));
        dereg_params.field_mask = UCT_MD_MEM_DEREG_FIELD_MEMH;
        dereg_params.memh       = memh1;
        tmp_status = uct_md_mem_dereg_v2(md, &dereg_params);
        if ((status == UCS_OK) && (tmp_status != UCS_OK)) {
            status = tmp_status;
        }
    }

    free(rkey_buf);
    free(buf2);
    free(buf1);

    if (iface != NULL) {
        uct_iface_close(iface);
    }
    if (iface_config != NULL) {
        uct_config_release(iface_config);
    }
    if (worker != NULL) {
        uct_worker_destroy(worker);
    }
    if (async != NULL) {
        ucs_async_context_destroy(async);
    }

    if (tl_resources != NULL) {
        uct_release_tl_resource_list(tl_resources);
    }
    if (md != NULL) {
        uct_md_close(md);
    }
    if (md_config != NULL) {
        uct_config_release(md_config);
    }

    free(md_resources);

    if (components != NULL) {
        uct_release_component_list(components);
    }

    if (status == UCS_OK) {
        printf("OVERALL: PASS\n");
        return 0;
    }

    printf("OVERALL: FAIL (%s)\n", ucs_status_string(status));
    return 1;
}
