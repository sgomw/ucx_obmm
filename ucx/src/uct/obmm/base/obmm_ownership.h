/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_OBMM_OWNERSHIP_H_
#define UCT_OBMM_OWNERSHIP_H_

#include "obmm_region.h"


ucs_status_t uct_obmm_region_set_ownership(uct_obmm_region_t *region, void *start,
                                           size_t length, int prot);


#endif
