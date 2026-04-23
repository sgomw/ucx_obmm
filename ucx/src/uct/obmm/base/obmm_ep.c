/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "obmm_ep.h"
#include "obmm_iface.h"


static UCS_CLASS_INIT_FUNC(uct_obmm_ep_t, const uct_ep_params_t *params)
{
    uct_obmm_iface_t *iface = ucs_derived_of(params->iface, uct_obmm_iface_t);

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super.super);
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_obmm_ep_t)
{
}

UCS_CLASS_DEFINE(uct_obmm_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_obmm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_obmm_ep_t, uct_ep_t);

int uct_obmm_ep_is_connected(const uct_ep_h tl_ep,
                             const uct_ep_is_connected_params_t *params)
{
    const uct_obmm_ep_t         *ep   = ucs_derived_of(tl_ep, uct_obmm_ep_t);
    const uct_obmm_iface_t      *iface;
    const uct_obmm_iface_addr_t *addr;

    if (!uct_base_ep_is_connected(tl_ep, params)) {
        return 0;
    }

    iface = ucs_derived_of(ep->super.super.iface, uct_obmm_iface_t);
    addr  = (const uct_obmm_iface_addr_t*)params->iface_addr;
    return (addr != NULL) && (*addr == iface->id);
}
