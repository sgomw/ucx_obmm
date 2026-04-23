/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_EP_H_
#define UCT_OBMM_EP_H_

#include <uct/base/uct_iface.h>


typedef struct uct_obmm_ep {
    uct_base_ep_t super;
} uct_obmm_ep_t;

UCS_CLASS_DECLARE_NEW_FUNC(uct_obmm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_obmm_ep_t, uct_ep_t);

int uct_obmm_ep_is_connected(const uct_ep_h tl_ep,
                             const uct_ep_is_connected_params_t *params);

#endif
